//////////////////////////////////////////////////////////////////////
//
//  LiquidViscosity.cpp - see LiquidViscosity.h
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LiquidViscosity.h"

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

enum Kind : std::uint8_t { Inactive = 0, Unknown = 1, Solid = 2, Wall = 3 };
constexpr int OwnerStatic = -1, OwnerWall = -2, OwnerNone = -3;

// One term of a one-sided difference: an unknown face or a boundary value.
struct Term {
	std::size_t f = 0;
	double coef = 0;
	bool unknown = false;
	double value = 0;
	int owner = OwnerNone;
	int body = -1;         // a coupled body: the value is its rigid velocity at `at`, unknown
	V3 at{{0, 0, 0}};
};
// d(rigid velocity component c at r from the centre)/d(V, Omega).
void RigidGradient(int c, const V3& r, double g[6])
{
	for (int k = 0; k < 6; ++k) g[k] = 0;
	g[c] = 1;
	// (Omega x r)_c: c = 0: Oy rz - Oz ry; 1: Oz rx - Ox rz; 2: Ox ry - Oy rx.
	if (c == 0) { g[4] = r[2]; g[5] = -r[1]; }
	else if (c == 1) { g[5] = r[0]; g[3] = -r[2]; }
	else { g[3] = r[1]; g[4] = -r[0]; }
}
struct Pair { int n = 0; Term t[2]; };

// Trilinear interpolation of the node array at x (clamped to the domain).
double NodeSample(const Grid& g, const std::vector<double>& nodes, const V3& x)
{
	const auto d = g.NodeDims();
	int i0[3];
	double w[3];
	for (int a = 0; a < 3; ++a) {
		double s = (x[a] - g.origin[a])/g.h;
		s = std::min(std::max(s, 0.0), double(g.n[a]));
		int i = int(std::floor(s));
		if (i >= g.n[a]) i = g.n[a] - 1;
		if (i < 0) i = 0;
		i0[a] = i;
		w[a] = s - i;
	}
	double v = 0;
	for (int m = 0; m < 8; ++m) {
		const int q[3] = {i0[0] + (m & 1), i0[1] + ((m >> 1) & 1), i0[2] + ((m >> 2) & 1)};
		double wt = 1;
		for (int a = 0; a < 3; ++a) wt *= (q[a] - i0[a]) ? w[a] : 1 - w[a];
		if (wt == 0) continue;
		v += wt*nodes[Index(d, std::min(q[0], d[0] - 1), std::min(q[1], d[1] - 1), std::min(q[2], d[2] - 1))];
	}
	return v;
}

double Clamp(double x, double lo, double hi) { return std::min(hi, std::max(lo, x)); }

class System {
public:
	System(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
		const ViscosityInput& input, const ViscosityOptions& o, double dt, const MacField& ustar, Pool* pool, double density = 1000)
		: density_(density), g_(g), phi_(phi), fraction_(fraction), in_(input), o_(o), ustar_(ustar), pool_(pool)
	{
		scale_ = 2*dt*o.kinematic;
		ih_ = 1/g.h;
		if (input.bodies) {
			for (const ViscousTwist& b : *input.bodies) {
				anyCoupled_ = anyCoupled_ || b.coupled;
				for (int k = 0; k < 3; ++k) wstar_.push_back(b.v[std::size_t(k)]);
				for (int k = 0; k < 3; ++k) wstar_.push_back(b.w[std::size_t(k)]);
			}
			wb_ = wstar_;
		}
		for (int a = 0; a < 3; ++a) {
			const auto d = g.FaceDims(a);
			faceStride_[a] = {{1, std::size_t(d[0]), std::size_t(d[0])*std::size_t(d[1])}};
		}
		for (int c = 0; c < 3; ++c) {
			const int a = (c + 1) % 3, b = (c + 2) % 3;
			edges_[c] = g.n[a] > 1 && g.n[b] > 1;
			ed_[c] = {{g.n[0] + 1, g.n[1] + 1, g.n[2] + 1}};
			ed_[c][c] = g.n[c];
		}
		Build();
	}

	std::size_t unknowns = 0;
	std::array<std::vector<std::uint8_t>,3> kind;
	std::array<std::vector<double>,3> mass, diag;

	// Strains (weighted control volumes only): cells (3 diagonal components), edges (3 sets).
	std::array<std::vector<double>,3> ec, ee;

	// The difference of component `comp` along `dir` between the face at `lo`
	// (indices on comp's face grid, possibly one outside along dir) and the next.
	Pair Diff(int comp, int dir, const int lo[3]) const
	{
		Pair p;
		const auto D = g_.FaceDims(comp);
		int hi[3] = {lo[0], lo[1], lo[2]};
		++hi[dir];
		const bool inLo = lo[dir] >= 0, inHi = hi[dir] <= D[dir] - 1;
		const double h = g_.h;
		auto centre = [&](const int q[3]) { return g_.FaceCentre(comp, q[0], q[1], q[2]); };
		auto owner = [&](std::size_t f) { return in_.owner ? int((*in_.owner)[comp][f]) : OwnerStatic; };
		auto constTerm = [&](std::size_t f, int k, double coef, const int q[3]) {
			Term t;
			t.f = f;
			t.coef = coef;
			t.at = centre(q);
			if (k == Inactive) { t.value = ustar_.u[comp][f]; t.owner = OwnerNone; }
			else if (k == Wall) { t.value = 0; t.owner = OwnerWall; }
			else {
				t.value = in_.solid ? in_.solid->u[comp][f] : 0.0;
				t.owner = owner(f);
				if (Coupled(t.owner)) t.body = t.owner;
			}
			return t;
		};
		auto sampleTerm = [&](std::size_t f, double coef, const int q[3]) {
			const int k = kind[comp][f];
			if (k == Unknown) { Term t; t.f = f; t.coef = coef; t.unknown = true; return t; }
			return constTerm(f, k, coef, q);
		};
		if (!inLo || !inHi) {
			// A tangential difference across a domain wall: zero at the wall, h/2 away.
			const int* q = inLo ? lo : hi;
			const std::size_t f = g_.F(comp, q[0], q[1], q[2]);
			const double c = 2/h;
			p.n = 2;
			p.t[0] = sampleTerm(f, inLo ? -c : c, q);
			Term w;
			w.coef = inLo ? c : -c;
			w.owner = OwnerWall;
			w.at = centre(q);
			w.at[dir] += (inLo ? 0.5 : -0.5)*h;
			p.t[1] = w;
			return p;
		}
		const std::size_t fl = g_.F(comp, lo[0], lo[1], lo[2]), fh = g_.F(comp, hi[0], hi[1], hi[2]);
		const int kl = kind[comp][fl], kh = kind[comp][fh];
		const bool sl = kl == Solid, sh = kh == Solid;
		p.n = 2;
		if (sl != sh && (sl ? kh : kl) != Wall) {
			// No slip at the solid crossing between a liquid face and a solid one.
			const std::size_t fo = sl ? fh : fl, fs = sl ? fl : fh;
			const int* qo = sl ? hi : lo;
			const double po = phis_[comp][fo], ps = phis_[comp][fs];
			const double theta = Clamp(po > ps ? po/(po - ps) : 1.0, o_.thetaMin, 1.0);
			const double c = 1/(theta*h);
			p.t[0] = sampleTerm(fo, sl ? c : -c, qo);
			Term w;
			w.coef = sl ? -c : c;
			w.owner = owner(fs);
			w.at = centre(qo);
			w.at[dir] += (sl ? -1.0 : 1.0)*theta*h;
			w.value = 0;
			if (Coupled(w.owner)) w.body = w.owner;
			else if (w.owner >= 0 && in_.bodies && std::size_t(w.owner) < in_.bodies->size()) {
				const ViscousTwist& b = (*in_.bodies)[std::size_t(w.owner)];
				const V3 r{{w.at[0] - b.x[0], w.at[1] - b.x[1], w.at[2] - b.x[2]}};
				const V3 wr{{b.w[1]*r[2] - b.w[2]*r[1], b.w[2]*r[0] - b.w[0]*r[2], b.w[0]*r[1] - b.w[1]*r[0]}};
				w.value = b.v[comp] + wr[comp];
			}
			p.t[1] = w;
			return p;
		}
		if (kl == Unknown && kh == Unknown) {
			// The common case: two unknowns.
			p.t[0].f = fl; p.t[0].coef = -1/h; p.t[0].unknown = true;
			p.t[1].f = fh; p.t[1].coef = 1/h; p.t[1].unknown = true;
			return p;
		}
		p.t[0] = sampleTerm(fl, -1/h, lo);
		p.t[1] = sampleTerm(fh, 1/h, hi);
		return p;
	}

	bool Coupled(int owner) const
	{
		return owner >= 0 && in_.bodies && std::size_t(owner) < in_.bodies->size() && (*in_.bodies)[std::size_t(owner)].coupled;
	}
	// A coupled body's rigid velocity at the term's point: free degrees of freedom
	// from the iterate wb_ (unknowns), locked ones from w* (constants).
	double BodyValue(const Term& t, int comp, bool unknowns, bool constants) const
	{
		const ViscousTwist& b = (*in_.bodies)[std::size_t(t.body)];
		const V3 r{{t.at[0] - b.x[0], t.at[1] - b.x[1], t.at[2] - b.x[2]}};
		double gr[6];
		RigidGradient(comp, r, gr);
		double v = 0;
		for (int k = 0; k < 6; ++k) {
			if (gr[k] == 0) continue;
			const bool free = b.free[std::size_t(k < 3 ? k : 3)];
			if (free && unknowns) v += gr[k]*wb_[6*std::size_t(t.body) + std::size_t(k)];
			else if (!free && constants) v += gr[k]*wstar_[6*std::size_t(t.body) + std::size_t(k)];
		}
		return v;
	}
	double Eval(const Pair& p, const std::array<std::vector<double>,3>& x, int comp, bool unknowns, bool constants) const
	{
		double s = 0;
		for (int m = 0; m < p.n; ++m) {
			const Term& t = p.t[m];
			if (t.unknown) { if (unknowns) s += t.coef*x[comp][t.f]; }
			else if (t.body >= 0) s += t.coef*BodyValue(t, comp, unknowns, constants);
			else if (constants) s += t.coef*t.value;
		}
		return s;
	}
	// Coupled bodies: their velocity unknowns (6 per body, free ones used) and w*.
	std::vector<double> wb_, wstar_;
	bool anyCoupled_ = false;
	// The strains with a coupled body's term: (kind 0 cell / 1 edge, axis or set, index, i, j, k).
	struct BodyStrain { int type, axis; std::size_t id; int idx[3]; };
	std::vector<BodyStrain> bodyStrains_;
	// out[6 i + k] = sum over the body strains of S d eps/dw_k eps (squares: of S (d eps/dw_k)^2).
	void GatherBodies(std::vector<double>& out, bool squares) const
	{
		const std::size_t nb = in_.bodies ? in_.bodies->size() : 0;
		out.assign(6*nb, 0.0);
		auto add = [&](const Pair& p, int comp, double weight, double strain) {
			for (int m = 0; m < p.n; ++m) {
				const Term& t = p.t[m];
				if (t.body < 0) continue;
				const ViscousTwist& b = (*in_.bodies)[std::size_t(t.body)];
				const V3 r{{t.at[0] - b.x[0], t.at[1] - b.x[1], t.at[2] - b.x[2]}};
				double gr[6];
				RigidGradient(comp, r, gr);
				for (int k = 0; k < 6; ++k) {
					if (gr[k] == 0 || !b.free[std::size_t(k < 3 ? k : 3)]) continue;
					const double cf = t.coef*gr[k];
					out[6*std::size_t(t.body) + std::size_t(k)] += squares ? weight*cf*cf : weight*cf*strain;
				}
			}
		};
		for (const BodyStrain& s : bodyStrains_) {
			if (s.type == 0) add(Diff(s.axis, s.axis, s.idx), s.axis, wc_[s.id], squares ? 0.0 : ec[s.axis][s.id]);
			else {
				const int c = s.axis, a = (c + 1) % 3, b = (c + 2) % 3;
				Pair d1, d2;
				EdgePairs(c, s.idx, d1, d2);
				// E = 2 w eps^2, eps = (d1 + d2) / 2: weight 2 w with the coefficient halved,
				// i.e. w / 2 per squared coefficient, w per coefficient times eps.
				const double strain = squares ? 0.0 : ee[c][s.id];
				add(d1, a, squares ? 0.5*we_[c][s.id] : we_[c][s.id], strain);
				add(d2, b, squares ? 0.5*we_[c][s.id] : we_[c][s.id], strain);
			}
		}
	}
	// y = M w / (rho h^3) for the free degrees of freedom.
	void BodyMass(const std::vector<double>& w, std::vector<double>& y, double density) const
	{
		const std::size_t nb = in_.bodies ? in_.bodies->size() : 0;
		y.assign(6*nb, 0.0);
		const double unit = density*g_.h*g_.h*g_.h;
		for (std::size_t i = 0; i < nb; ++i) {
			const ViscousTwist& b = (*in_.bodies)[i];
			if (!b.coupled) continue;
			for (int k = 0; k < 3; ++k) if (b.free[std::size_t(k)]) y[6*i + std::size_t(k)] = b.mass*w[6*i + std::size_t(k)]/unit;
			if (b.free[3])
				for (int r = 0; r < 3; ++r) {
					double s = 0;
					for (int c = 0; c < 3; ++c) s += b.inertia[std::size_t(3*r + c)]*w[6*i + 3 + std::size_t(c)];
					y[6*i + 3 + std::size_t(r)] = s/unit;
				}
		}
	}
	static double CoefOf(const Pair& p, std::size_t f)
	{
		double c = 0;
		for (int m = 0; m < p.n; ++m) if (p.t[m].unknown && p.t[m].f == f) c += p.t[m].coef;
		return c;
	}

	// Edge e of set c: the two differences (component a along b, component b along a).
	void EdgePairs(int c, const int e[3], Pair& d1, Pair& d2) const
	{
		const int a = (c + 1) % 3, b = (c + 2) % 3;
		int q[3] = {e[0], e[1], e[2]};
		--q[b];
		d1 = Diff(a, b, q);
		int r[3] = {e[0], e[1], e[2]};
		--r[a];
		d2 = Diff(b, a, r);
	}

	void Strains(const std::array<std::vector<double>,3>& x, bool unknowns, bool constants)
	{
		const auto& n = g_.n;
		for (int a = 0; a < 3; ++a) {
			ec[a].assign(g_.Cells(), 0.0);
			if (n[a] == 1) continue;
			ForRows(pool_, RowsOf(n), RowGrain(n[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(n[1])), k = int(r/std::size_t(n[1]));
					for (int i = 0; i < n[0]; ++i) {
						const std::size_t cc = g_.C(i, j, k);
						if (!(wc_[cc] > 0)) continue;
						const int lo[3] = {i, j, k};
						const std::size_t f0 = g_.F(a, i, j, k), f1 = f0 + faceStride_[a][a];
						if (kind[a][f0] == Unknown && kind[a][f1] == Unknown) { ec[a][cc] = unknowns ? (x[a][f1] - x[a][f0])*ih_ : 0.0; continue; }
						ec[a][cc] = Eval(Diff(a, a, lo), x, a, unknowns, constants);
					}
				}
			});
		}
		for (int c = 0; c < 3; ++c) {
			if (!edges_[c]) continue;
			const int a = (c + 1) % 3, b = (c + 2) % 3;
			const auto& d = ed_[c];
			ee[c].assign(std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]), 0.0);
			ForRows(pool_, RowsOf(d), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
					for (int i = 0; i < d[0]; ++i) {
						const std::size_t id = Index(d, i, j, k);
						if (!(we_[c][id] > 0)) continue;
						const int e[3] = {i, j, k};
						if (e[a] > 0 && e[a] < n[a] && e[b] > 0 && e[b] < n[b]) {
							// Interior edge: the four samples are faces; fast path when all are unknowns.
							const std::size_t fa1 = g_.F(a, e[0], e[1], e[2]), fa0 = fa1 - faceStride_[a][b];
							const std::size_t fb1 = g_.F(b, e[0], e[1], e[2]), fb0 = fb1 - faceStride_[b][a];
							if (kind[a][fa0] == Unknown && kind[a][fa1] == Unknown && kind[b][fb0] == Unknown && kind[b][fb1] == Unknown) {
								ee[c][id] = unknowns ? 0.5*((x[a][fa1] - x[a][fa0]) + (x[b][fb1] - x[b][fb0]))*ih_ : 0.0;
								continue;
							}
						}
						Pair d1, d2;
						EdgePairs(c, e, d1, d2);
						ee[c][id] = 0.5*(Eval(d1, x, a, unknowns, constants) + Eval(d2, x, b, unknowns, constants));
					}
				}
			});
		}
	}

	// For every unknown face: sum over its strains of S d(eps)/du_f eps (and,
	// with `squares`, of S (d eps/du_f)^2 into out instead).
	void Gather(std::array<std::vector<double>,3>& out, bool squares) const
	{
		const auto& n = g_.n;
		for (int a = 0; a < 3; ++a) {
			out[a].assign(g_.Faces(a), 0.0);
			if (n[a] == 1) continue;
			const auto D = g_.FaceDims(a);
			ForRows(pool_, RowsOf(D), RowGrain(D[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(D[1])), k = int(r/std::size_t(D[1]));
					for (int i = 0; i < D[0]; ++i) {
						const std::size_t f = Index(D, i, j, k);
						if (kind[a][f] != Unknown) continue;
						const int idx[3] = {i, j, k};
						double s = 0;
						// Cell strains: f is the low face of cell idx and the high face of idx - e_a.
						for (int side = 0; side < 2; ++side) {
							int cidx[3] = {i, j, k};
							cidx[a] -= side;
							if (cidx[a] < 0 || cidx[a] >= n[a]) continue;
							const std::size_t cc = g_.C(cidx[0], cidx[1], cidx[2]);
							const double w = wc_[cc];
							if (!(w > 0)) continue;
							const std::size_t other = side ? f - faceStride_[a][a] : f + faceStride_[a][a];
							const double cf = kind[a][other] == Unknown ? (side ? ih_ : -ih_) : CoefOf(Diff(a, a, cidx), f);
							s += squares ? w*cf*cf : w*cf*ec[a][cc];
						}
						// Edge strains of the sets c != a with both other axes resolved.
						for (int b = 0; b < 3; ++b) {
							if (b == a || n[b] == 1) continue;
							const int c = 3 - a - b;
							if (!edges_[c]) continue;
							for (int side = 0; side < 2; ++side) {
								int e[3] = {idx[0], idx[1], idx[2]};
								e[b] += 1 - side;   // f is the low sample of the edge at node idx[b] + 1, the high one at idx[b]
								const std::size_t id = Index(ed_[c], e[0], e[1], e[2]);
								const double w = we_[c][id];
								if (!(w > 0)) continue;
								int lo[3] = {e[0], e[1], e[2]};
								--lo[b];
								// f's partner in the difference along b (the other side of the edge).
								const bool inside = side ? e[b] >= 1 && idx[b] >= 1 : e[b] <= n[b] - 1 && idx[b] + 1 <= n[b] - 1;
								double cf;
								if (inside && kind[a][side ? f - faceStride_[a][b] : f + faceStride_[a][b]] == Unknown) cf = side ? 0.5*ih_ : -0.5*ih_;
								else cf = 0.5*CoefOf(Diff(a, b, lo), f);
								s += squares ? 2*w*cf*cf : 2*w*cf*ee[c][id];
							}
						}
						out[a][f] = s;
					}
				}
			});
		}
	}

	void Apply(const std::array<std::vector<double>,3>& x, std::array<std::vector<double>,3>& y)
	{
		Strains(x, true, false);
		Gather(y, false);
		for (int a = 0; a < 3; ++a) {
			if (g_.n[a] == 1) continue;
			std::vector<double>& ya = y[a];
			const std::vector<double>& xa = x[a];
			const std::vector<double>& m = mass[a];
			const std::vector<std::uint8_t>& kd = kind[a];
			ForRows(pool_, ya.size(), 4096, [&](std::size_t f0, std::size_t f1) {
				for (std::size_t f = f0; f < f1; ++f) ya[f] = kd[f] == Unknown ? m[f]*xa[f] + scale_*ya[f] : 0.0;
			});
		}
	}

	// The impulse on the liquid from each boundary value's owner (after Strains with
	// the solution and constants): per body force and torque about its centre.
	void Attribute(std::size_t bodies, std::vector<Impulse6>& onBodies, V3& walls, double density)
	{
		const auto& n = g_.n;
		const std::size_t slots = bodies + 1;   // bodies, then static solids and walls
		const double k = scale_*density*g_.h*g_.h*g_.h;   // 2 dt mu h^3
		std::vector<std::array<double,6>> total(slots, std::array<double,6>{{0, 0, 0, 0, 0, 0}});
		auto add = [&](std::vector<std::array<double,6>>& acc, const Pair& p, int comp, double weightedStrain) {
			for (int m = 0; m < p.n; ++m) {
				const Term& t = p.t[m];
				if (t.unknown || t.owner == OwnerNone || t.body >= 0) continue;
				const double F = k*weightedStrain*t.coef;    // impulse on the liquid along comp
				if (t.owner >= 0 && std::size_t(t.owner) < bodies) {
					std::array<double,6>& s = acc[std::size_t(t.owner)];
					s[std::size_t(comp)] += F;
					const ViscousTwist& b = (*in_.bodies)[std::size_t(t.owner)];
					const V3 r{{t.at[0] - b.x[0], t.at[1] - b.x[1], t.at[2] - b.x[2]}};
					V3 e{{0, 0, 0}};
					e[std::size_t(comp)] = F;
					s[3] += r[1]*e[2] - r[2]*e[1];
					s[4] += r[2]*e[0] - r[0]*e[2];
					s[5] += r[0]*e[1] - r[1]*e[0];
				} else acc[bodies][std::size_t(comp)] += F;
			}
		};
		// Cells.
		for (int a = 0; a < 3; ++a) {
			if (n[a] == 1) continue;
			const std::size_t rows = RowsOf(n), grain = RowGrain(n[0]), chunks = Cloth::Chunks(rows, grain);
			std::vector<std::vector<std::array<double,6>>> part(chunks, std::vector<std::array<double,6>>(slots, std::array<double,6>{{0, 0, 0, 0, 0, 0}}));
			Cloth::ParallelFor(pool_, chunks, [&](std::size_t ch) {
				for (std::size_t r = ch*grain; r < std::min(rows, (ch + 1)*grain); ++r) {
					const int j = int(r % std::size_t(n[1])), kk = int(r/std::size_t(n[1]));
					for (int i = 0; i < n[0]; ++i) {
						const std::size_t cc = g_.C(i, j, kk);
						if (!(wc_[cc] > 0) || ec[a][cc] == 0) continue;
						const int lo[3] = {i, j, kk};
						add(part[ch], Diff(a, a, lo), a, wc_[cc]*ec[a][cc]);
					}
				}
			});
			for (const auto& pc : part) for (std::size_t s = 0; s < slots; ++s) for (int m = 0; m < 6; ++m) total[s][std::size_t(m)] += pc[s][std::size_t(m)];
		}
		// Edges: E = 2 w eps^2 with eps = (d1 + d2) / 2: dE/dv = 2 (2 w eps) (1/2) coef.
		for (int c = 0; c < 3; ++c) {
			if (!edges_[c]) continue;
			const int a = (c + 1) % 3, b = (c + 2) % 3;
			const auto& d = ed_[c];
			const std::size_t rows = RowsOf(d), grain = RowGrain(d[0]), chunks = Cloth::Chunks(rows, grain);
			std::vector<std::vector<std::array<double,6>>> part(chunks, std::vector<std::array<double,6>>(slots, std::array<double,6>{{0, 0, 0, 0, 0, 0}}));
			Cloth::ParallelFor(pool_, chunks, [&](std::size_t ch) {
				for (std::size_t r = ch*grain; r < std::min(rows, (ch + 1)*grain); ++r) {
					const int j = int(r % std::size_t(d[1])), kk = int(r/std::size_t(d[1]));
					for (int i = 0; i < d[0]; ++i) {
						const std::size_t id = Index(d, i, j, kk);
						if (!(we_[c][id] > 0) || ee[c][id] == 0) continue;
						const int e[3] = {i, j, kk};
						Pair d1, d2;
						EdgePairs(c, e, d1, d2);
						const double ws = we_[c][id]*ee[c][id];   // 2 w eps * 1/2
						add(part[ch], d1, a, ws);
						add(part[ch], d2, b, ws);
					}
				}
			});
			for (const auto& pc : part) for (std::size_t s = 0; s < slots; ++s) for (int m = 0; m < 6; ++m) total[s][std::size_t(m)] += pc[s][std::size_t(m)];
		}
		onBodies.assign(bodies, Impulse6{{0, 0, 0, 0, 0, 0}});
		for (std::size_t i = 0; i < bodies; ++i) for (int m = 0; m < 6; ++m) onBodies[i][std::size_t(m)] = -total[i][std::size_t(m)];
		walls = {{total[bodies][0], total[bodies][1], total[bodies][2]}};
	}

	double Scale() const { return scale_; }

private:
	void Build()
	{
		const Grid& g = g_;
		const auto& n = g.n;
		Pool* pool = pool_;
		const bool solids = o_.noSlip && in_.solidNodes;
		const std::vector<double>* nodes = in_.solidNodes;
		// 1. Face kinds (unknown candidates for now) and solid distances at face centres.
		for (int a = 0; a < 3; ++a) {
			const auto D = g.FaceDims(a);
			kind[a].assign(g.Faces(a), Inactive);
			if (solids) phis_[a].assign(g.Faces(a), 1.0);
			ForRows(pool, RowsOf(D), RowGrain(D[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(D[1])), k = int(r/std::size_t(D[1]));
					for (int i = 0; i < D[0]; ++i) {
						const std::size_t f = Index(D, i, j, k);
						const int idx[3] = {i, j, k};
						if (idx[a] == 0 || idx[a] == n[a]) {
							kind[a][f] = (o_.openTop && a == 1 && idx[a] == n[a] && n[a] > 1) ? Unknown : Wall;
							continue;
						}
						if (solids) {
							const int b = (a + 1) % 3, c = (a + 2) % 3;
							double s = 0;
							for (int m = 0; m < 4; ++m) {
								int q[3] = {i, j, k};
								if (m == 1 || m == 2) ++q[b];
								if (m == 2 || m == 3) ++q[c];
								s += (*nodes)[g.N(q[0], q[1], q[2])];
							}
							phis_[a][f] = 0.25*s;
							if (phis_[a][f] < 0) { kind[a][f] = Solid; continue; }
						}
						kind[a][f] = Unknown;
					}
				}
			});
		}
		// 2. Control-volume weights: liquid fraction outside the solids (2 samples per resolved axis).
		// S^d samples of clamp(1/2 - phi / h_s) clamp(1/2 + phi_s / h_s), h_s = h / S (S per
		// resolved axis: o_.cvSamples where the volume is cut, 1 where it is not).
		auto liquidOpen = [&](const V3& centre) {
			const double reach = 0.8660254037844386*g.h;
			const double pc = SampleCells(g, phi_, centre);
			if (pc > reach + 0.5*g.h) return 0.0;
			const double sc = nodes ? NodeSample(g, *nodes, centre) : 1e300;
			if (sc < -reach) return 0.0;
			bool atWall = false;
			for (int a = 0; a < 3; ++a)
				if (n[a] > 1 && (centre[a] - 0.5*g.h < g.origin[a] - 1e-12*g.h || centre[a] + 0.5*g.h > g.origin[a] + n[a]*g.h + 1e-12*g.h)) atWall = true;
			if (pc < -reach && sc > reach && !atWall) return 1.0;
			const int S = std::max(1, o_.cvSamples);
			const double hs = g.h/S;
			const int Sa[3] = {n[0] > 1 ? S : 1, n[1] > 1 ? S : 1, n[2] > 1 ? S : 1};
			double sum = 0;
			int count = 0;
			for (int c3 = 0; c3 < Sa[2]; ++c3) for (int b3 = 0; b3 < Sa[1]; ++b3) for (int a3 = 0; a3 < Sa[0]; ++a3) {
				const int q[3] = {a3, b3, c3};
				V3 x = centre;
				bool outside = false;
				for (int a = 0; a < 3; ++a) {
					if (n[a] == 1) continue;
					x[a] += ((q[a] + 0.5)/S - 0.5)*g.h;
					if (x[a] < g.origin[a] || x[a] > g.origin[a] + n[a]*g.h) outside = true;
				}
				++count;
				if (outside) continue;
				const double liquid = Clamp(0.5 - SampleCells(g, phi_, x)/hs, 0.0, 1.0);
				if (liquid == 0) continue;
				const double open = nodes ? Clamp(0.5 + NodeSample(g, *nodes, x)/hs, 0.0, 1.0) : 1.0;
				sum += liquid*open;
			}
			return sum/count;
		};
		wc_.assign(g.Cells(), 0.0);
		ForRows(pool, RowsOf(n), RowGrain(n[0]), [&](std::size_t r0, std::size_t r1) {
			for (std::size_t r = r0; r < r1; ++r) {
				const int j = int(r % std::size_t(n[1])), k = int(r/std::size_t(n[1]));
				for (int i = 0; i < n[0]; ++i) {
					const std::size_t c = g.C(i, j, k);
					if (phi_[c] > 1.8*g.h) continue;
					wc_[c] = liquidOpen(g.CellCentre(i, j, k));
				}
			}
		});
		for (int c = 0; c < 3; ++c) {
			if (!edges_[c]) continue;
			const int a = (c + 1) % 3, b = (c + 2) % 3;
			const auto& d = ed_[c];
			we_[c].assign(std::size_t(d[0])*std::size_t(d[1])*std::size_t(d[2]), 0.0);
			ForRows(pool, RowsOf(d), RowGrain(d[0]), [&](std::size_t r0, std::size_t r1) {
				for (std::size_t r = r0; r < r1; ++r) {
					const int j = int(r % std::size_t(d[1])), k = int(r/std::size_t(d[1]));
					for (int i = 0; i < d[0]; ++i) {
						const int e[3] = {i, j, k};
						const bool wallA = e[a] == 0 || e[a] == n[a], wallB = e[b] == 0 || e[b] == n[b];
						if (!o_.noSlip && (wallA || wallB)) continue;
						if (o_.openTop && ((a == 1 && e[a] == n[1]) || (b == 1 && e[b] == n[1]))) continue;
						V3 x;
						for (int q = 0; q < 3; ++q) x[q] = g.origin[q] + (q == c ? e[q] + 0.5 : double(e[q]))*g.h;
						// Skip edges far from the liquid (phi at the nearest cell).
						int ci[3];
						for (int q = 0; q < 3; ++q) ci[q] = std::min(std::max(q == c ? e[q] : e[q] - 1, 0), n[q] - 1);
						bool nearLiquid = false;  // not `near`: an MSVC <windows.h> macro
						for (int m = 0; m < 4 && !nearLiquid; ++m) {
							int q2[3] = {ci[0], ci[1], ci[2]};
							if (m & 1) q2[a] = std::min(e[a], n[a] - 1);
							if (m & 2) q2[b] = std::min(e[b], n[b] - 1);
							if (phi_[g.C(q2[0], q2[1], q2[2])] < 1.8*g.h) nearLiquid = true;
						}
						if (!nearLiquid) continue;
						we_[c][Index(d, i, j, k)] = liquidOpen(x);
					}
				}
			});
		}
		// 3. Unknowns: candidates with kinetic weight or in a weighted strain; masses.
		std::array<std::vector<double>,3> sq;
		Gather(sq, true);
		unknowns = 0;
		for (int a = 0; a < 3; ++a) {
			const auto D = g.FaceDims(a);
			mass[a].assign(g.Faces(a), 0.0);
			if (n[a] == 1) { std::fill(kind[a].begin(), kind[a].end(), std::uint8_t(Wall)); continue; }
			for (int k = 0; k < D[2]; ++k) for (int j = 0; j < D[1]; ++j) for (int i = 0; i < D[0]; ++i) {
				const std::size_t f = Index(D, i, j, k);
				if (kind[a][f] != Unknown) continue;
				const int idx[3] = {i, j, k};
				double w = 0;
				const double A = fraction_[a][f];
				if (A > 0) {
					int lo[3] = {i, j, k};
					--lo[a];
					const bool hasL = idx[a] > 0, hasR = idx[a] < n[a];
					const double pl = hasL ? phi_[g.C(lo[0], lo[1], lo[2])] : 1.0, pr = hasR ? phi_[g.C(i, j, k)] : 1.0;
					if (hasL && hasR) {
						if (pl < 0 && pr < 0) w = A;
						else if (pl < 0) w = A*Clamp(pl/(pl - pr), o_.massThetaMin, 1.0);
						else if (pr < 0) w = A*Clamp(pr/(pr - pl), o_.massThetaMin, 1.0);
					} else if (hasL && pl < 0) w = A*Clamp(-pl/g.h, o_.massThetaMin, 0.5);   // open top
				}
				if (!(w > 0) && !(sq[a][f] > 0)) { kind[a][f] = Inactive; continue; }
				mass[a][f] = w > 0 ? w : o_.massFloor;
				++unknowns;
			}
		}
		// The diagonal (demoting a face changes no other face's coefficients).
		for (int a = 0; a < 3; ++a) {
			diag[a].assign(g.Faces(a), 1.0);
			for (std::size_t f = 0; f < diag[a].size(); ++f) if (kind[a][f] == Unknown) diag[a][f] = mass[a][f] + scale_*sq[a][f];
		}
		// 4. Coupled bodies: the strains that reach them, and their diagonal.
		if (anyCoupled_) {
			for (int a = 0; a < 3; ++a) {
				if (n[a] == 1) continue;
				for (int k = 0; k < n[2]; ++k) for (int j = 0; j < n[1]; ++j) for (int i = 0; i < n[0]; ++i) {
					const std::size_t cc = g.C(i, j, k);
					if (!(wc_[cc] > 0)) continue;
					const int lo[3] = {i, j, k};
					const Pair p = Diff(a, a, lo);
					if (p.t[0].body >= 0 || p.t[1].body >= 0) bodyStrains_.push_back({0, a, cc, {i, j, k}});
				}
			}
			for (int c = 0; c < 3; ++c) {
				if (!edges_[c]) continue;
				const auto& d = ed_[c];
				for (int k = 0; k < d[2]; ++k) for (int j = 0; j < d[1]; ++j) for (int i = 0; i < d[0]; ++i) {
					const std::size_t id = Index(d, i, j, k);
					if (!(we_[c][id] > 0)) continue;
					const int e[3] = {i, j, k};
					Pair d1, d2;
					EdgePairs(c, e, d1, d2);
					if (d1.t[0].body >= 0 || d1.t[1].body >= 0 || d2.t[0].body >= 0 || d2.t[1].body >= 0) bodyStrains_.push_back({1, c, id, {i, j, k}});
				}
			}
			std::vector<double> bsq, unit(wstar_.size(), 0.0), bm;
			GatherBodies(bsq, true);
			bodyDiag.assign(wstar_.size(), 1.0);
			for (std::size_t q = 0; q < wstar_.size(); ++q) {
				unit.assign(wstar_.size(), 0.0);
				unit[q] = 1;
				BodyMass(unit, bm, density_);
				if (bm[q] > 0) bodyDiag[q] = bm[q] + scale_*bsq[q];
			}
		}
	}
public:
	std::vector<double> bodyDiag;
	double density_ = 1000;
	const std::vector<double>& WStar() const { return wstar_; }
	std::vector<double>& WB() { return wb_; }
	bool AnyCoupled() const { return anyCoupled_; }
	bool FreeDof(std::size_t q) const
	{
		const ViscousTwist& b = (*in_.bodies)[q/6];
		const std::size_t k = q % 6;
		return b.coupled && b.free[k < 3 ? k : 3];
	}
private:

	const Grid& g_;
	const std::vector<double>& phi_;
	const std::array<std::vector<double>,3>& fraction_;
	const ViscosityInput& in_;
	const ViscosityOptions& o_;
	const MacField& ustar_;
	Pool* pool_;
	double scale_ = 0, ih_ = 1;
	std::array<std::array<std::size_t,3>,3> faceStride_;   // [component][axis]
	bool edges_[3] = {false, false, false};
	std::array<std::array<int,3>,3> ed_;
	std::array<std::vector<double>,3> phis_, we_;
	std::vector<double> wc_;
};

double DotFaces(Pool* pool, const std::array<std::vector<double>,3>& x, const std::array<std::vector<double>,3>& y)
{
	double s = 0;
	for (int a = 0; a < 3; ++a) if (!x[a].empty()) s += Dot(pool, x[a], y[a]);
	return s;
}
double MaxFaces(Pool* pool, const std::array<std::vector<double>,3>& x)
{
	double m = 0;
	for (int a = 0; a < 3; ++a) {
		const auto& v = x[a];
		m = std::max(m, MaxRows(pool, v.size(), 4096, [&](std::size_t i0, std::size_t i1) {
			double t = 0;
			for (std::size_t i = i0; i < i1; ++i) t = std::max(t, std::fabs(v[i]));
			return t;
		}));
	}
	return m;
}
} // namespace

bool ViscousStep(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const ViscosityInput& input, const ViscosityOptions& options, double dt, double density, MacField& u,
	std::vector<Impulse6>& bodyImpulse, V3& wallImpulse, ViscosityStats& stats, Pool* pool, std::string& error)
{
	const double start = Now();
	stats = ViscosityStats();
	const std::size_t nb = input.bodies ? input.bodies->size() : 0;
	bodyImpulse.assign(nb, Impulse6{{0, 0, 0, 0, 0, 0}});
	wallImpulse = {{0, 0, 0}};
	if (!(options.kinematic > 0) || !(dt > 0)) { stats.converged = true; return true; }
	const MacField ustar = u;
	// Grids below 32k cells run on the caller: a CG iteration has some twenty
	// parallel loops, and their joins cost more than the work there.
	if (g.Cells() < 32768) pool = nullptr;
	System s(g, phi, fraction, input, options, dt, ustar, pool, density);
	stats.build = Now() - start;
	stats.unknowns = s.unknowns;
	if (s.unknowns == 0) { stats.converged = true; return true; }
	// The unknowns: the faces, and the free degrees of freedom of coupled bodies
	// (6 per body; the others stay zero).
	const std::size_t nq = 6*nb;
	const std::vector<double>& wstar = s.WStar();
	auto apply = [&](const std::array<std::vector<double>,3>& x, const std::vector<double>& xb,
		std::array<std::vector<double>,3>& y, std::vector<double>& yb) {
		if (s.AnyCoupled()) s.WB() = xb;
		s.Apply(x, y);
		yb.assign(nq, 0.0);
		if (!s.AnyCoupled()) return;
		std::vector<double> gb, mb;
		s.GatherBodies(gb, false);
		s.BodyMass(xb, mb, density);
		for (std::size_t q = 0; q < nq; ++q) yb[q] = s.FreeDof(q) ? mb[q] + s.Scale()*gb[q] : 0.0;
	};
	auto dot = [&](const std::array<std::vector<double>,3>& x, const std::vector<double>& xb,
		const std::array<std::vector<double>,3>& y, const std::vector<double>& yb) {
		double d = DotFaces(pool, x, y);
		for (std::size_t q = 0; q < nq; ++q) d += xb[q]*yb[q];
		return d;
	};
	auto norm = [&](const std::array<std::vector<double>,3>& x, const std::vector<double>& xb) {
		double m = MaxFaces(pool, x);
		for (std::size_t q = 0; q < nq; ++q) m = std::max(m, std::fabs(xb[q]));
		return m;
	};
	// b = M u* - 2 dt nu G^T S g0 (and M w* - 2 dt nu G_w^T S g0 for the bodies).
	std::array<std::vector<double>,3> b, x, r, z, p, Ap;
	std::vector<double> bb(nq, 0.0), xb(nq, 0.0), rb(nq, 0.0), zb(nq, 0.0), pb(nq, 0.0), Apb(nq, 0.0);
	s.Strains(ustar.u, false, true);
	s.Gather(b, false);
	if (s.AnyCoupled()) {
		std::vector<double> gb, mb;
		s.GatherBodies(gb, false);
		s.BodyMass(wstar, mb, density);
		for (std::size_t q = 0; q < nq; ++q) if (s.FreeDof(q)) { bb[q] = mb[q] - s.Scale()*gb[q]; xb[q] = wstar[q]; }
	}
	for (int a = 0; a < 3; ++a) {
		x[a].assign(g.Faces(a), 0.0);
		for (std::size_t f = 0; f < b[a].size(); ++f) {
			if (s.kind[a][f] != 1) { b[a][f] = 0; continue; }
			b[a][f] = s.mass[a][f]*ustar.u[a][f] - s.Scale()*b[a][f];
			x[a][f] = ustar.u[a][f];
		}
	}
	const double bnorm = norm(b, bb);
	if (bnorm > 0) {
		apply(x, xb, Ap, Apb);
		for (int a = 0; a < 3; ++a) {
			r[a].assign(g.Faces(a), 0.0);
			z[a].assign(g.Faces(a), 0.0);
			for (std::size_t f = 0; f < r[a].size(); ++f) if (s.kind[a][f] == 1) { r[a][f] = b[a][f] - Ap[a][f]; z[a][f] = r[a][f]/s.diag[a][f]; }
			p[a] = z[a];
		}
		for (std::size_t q = 0; q < nq; ++q) if (s.FreeDof(q)) { rb[q] = bb[q] - Apb[q]; zb[q] = rb[q]/s.bodyDiag[q]; }
		pb = zb;
		double rz = dot(r, rb, z, zb);
		const double target = options.tolerance*bnorm;
		double rnorm = norm(r, rb);
		int it = 0;
		while (rnorm > target && it < options.maxIterations) {
			apply(p, pb, Ap, Apb);
			const double pAp = dot(p, pb, Ap, Apb);
			if (!(pAp > 0)) break;
			const double alpha = rz/pAp;
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] == 1) continue;
				auto& xa = x[a]; auto& ra = r[a]; const auto& pa = p[a]; const auto& Apa = Ap[a];
				ForRows(pool, xa.size(), 4096, [&](std::size_t f0, std::size_t f1) {
					for (std::size_t f = f0; f < f1; ++f) { xa[f] += alpha*pa[f]; ra[f] -= alpha*Apa[f]; }
				});
			}
			for (std::size_t q = 0; q < nq; ++q) { xb[q] += alpha*pb[q]; rb[q] -= alpha*Apb[q]; }
			++it;
			rnorm = norm(r, rb);
			if (rnorm <= target) break;
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] == 1) continue;
				auto& za = z[a]; const auto& ra = r[a]; const auto& da = s.diag[a]; const auto& kd = s.kind[a];
				ForRows(pool, za.size(), 4096, [&](std::size_t f0, std::size_t f1) {
					for (std::size_t f = f0; f < f1; ++f) za[f] = kd[f] == 1 ? ra[f]/da[f] : 0.0;
				});
			}
			for (std::size_t q = 0; q < nq; ++q) zb[q] = s.FreeDof(q) ? rb[q]/s.bodyDiag[q] : 0.0;
			const double rzNew = dot(r, rb, z, zb);
			const double beta = rzNew/rz;
			rz = rzNew;
			for (int a = 0; a < 3; ++a) {
				if (g.n[a] == 1) continue;
				auto& pa = p[a]; const auto& za = z[a];
				ForRows(pool, pa.size(), 4096, [&](std::size_t f0, std::size_t f1) {
					for (std::size_t f = f0; f < f1; ++f) pa[f] = za[f] + beta*pa[f];
				});
			}
			for (std::size_t q = 0; q < nq; ++q) pb[q] = zb[q] + beta*pb[q];
		}
		stats.iterations = it;
		stats.residual = rnorm/bnorm;
		stats.converged = rnorm <= target;
	} else stats.converged = true;
	if (!stats.converged) {
		error = "viscosity solve did not converge (" + std::to_string(stats.iterations) + " iterations, residual " +
			std::to_string(stats.residual) + ")";
		return false;
	}
	for (int a = 0; a < 3; ++a)
		for (std::size_t f = 0; f < u.u[a].size(); ++f) if (s.kind[a][f] == 1) u.u[a][f] = x[a][f];
	// Forces: the strains of the solution with the boundary values.
	if (s.AnyCoupled()) s.WB() = xb;
	s.Strains(x, true, true);
	s.Attribute(nb, bodyImpulse, wallImpulse, density);
	if (s.AnyCoupled()) {
		// A coupled body's impulse: M (w - w*) on its free degrees of freedom.
		std::vector<double> dw(nq, 0.0), mdw;
		for (std::size_t q = 0; q < nq; ++q) if (s.FreeDof(q)) dw[q] = xb[q] - wstar[q];
		s.BodyMass(dw, mdw, density);
		const double unit = density*g.h*g.h*g.h;
		for (std::size_t i = 0; i < nb; ++i) {
			if (!(*input.bodies)[i].coupled) continue;
			for (int k = 0; k < 6; ++k) bodyImpulse[i][std::size_t(k)] = mdw[6*i + std::size_t(k)]*unit;
		}
	}
	stats.seconds = Now() - start;
	return true;
}

void ApplyViscousOperator(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const ViscosityInput& input, const ViscosityOptions& options, double dt, const MacField& x, MacField& y,
	FaceMask& unknown, Pool* pool)
{
	System s(g, phi, fraction, input, options, dt, x, pool);
	s.Apply(x.u, y.u);
	for (int a = 0; a < 3; ++a) {
		unknown[a].assign(g.Faces(a), 0);
		for (std::size_t f = 0; f < unknown[a].size(); ++f) unknown[a][f] = s.kind[a][f] == 1;
	}
}

} } }
