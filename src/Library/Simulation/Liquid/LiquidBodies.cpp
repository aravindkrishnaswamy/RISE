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
	return t == BodyShape::Type::Sphere ? "sphere" : t == BodyShape::Type::Box ? "box" : "cylinder";
}

double BodyShape::Distance(const V3& y) const
{
	switch (type) {
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
	}
	return 0;
}

double BodyShape::Volume() const
{
	switch (type) {
	case Type::Sphere: return 4.0/3.0*Pi*radius*radius*radius;
	case Type::Box: return size[0]*size[1]*size[2];
	case Type::Cylinder: return Pi*radius*radius*height;
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
	}
	return {{0, 0, 0}};
}

double BodyShape::SmallestExtent() const
{
	switch (type) {
	case Type::Sphere: return 2*radius;
	case Type::Box: return std::min(size[0], std::min(size[1], size[2]));
	case Type::Cylinder: return std::min(2*radius, height);
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
	// a bobbing body (docs/SIMULATION_M7B_RESULTS.md section 3).
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
