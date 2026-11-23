//////////////////////////////////////////////////////////////////////
//
//  ClothModel.cpp - Structured sheet, cloth energies, implicit integrators
//  (simulation M3a).  Formulas: ClothModel.h and
//  docs/SIMULATION_M3A_RESULTS.md.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothModel.h"
#include "ClothContact.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace RISE { namespace Simulation { namespace Cloth {

namespace {
constexpr double Pi = 3.14159265358979323846;

inline V3 Add(const V3& a, const V3& b) { return {{a[0]+b[0], a[1]+b[1], a[2]+b[2]}}; }
inline V3 Sub(const V3& a, const V3& b) { return {{a[0]-b[0], a[1]-b[1], a[2]-b[2]}}; }
inline V3 Mul(const V3& a, double s) { return {{a[0]*s, a[1]*s, a[2]*s}}; }
inline double Dot(const V3& a, const V3& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline V3 Cross(const V3& a, const V3& b)
{
	return {{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}};
}
inline V3 At(const std::vector<double>& x, std::uint32_t i)
{
	const std::size_t k = 3*static_cast<std::size_t>(i);
	return {{x[k], x[k+1], x[k+2]}};
}
inline void Put(std::vector<double>& x, std::uint32_t i, const V3& p)
{
	const std::size_t k = 3*static_cast<std::size_t>(i);
	x[k] = p[0]; x[k+1] = p[1]; x[k+2] = p[2];
}
inline void Accumulate(std::vector<double>& x, std::uint32_t i, const V3& p)
{
	const std::size_t k = 3*static_cast<std::size_t>(i);
	x[k] += p[0]; x[k+1] += p[1]; x[k+2] += p[2];
}

// Membrane element: energy, gradient (9) and projected Hessian (81, row-major over 3x3 vertex blocks).
// `allowance` limits the compressive stress kept in the geometric Hessian:
// an eigenvalue l < 0 of S with eigenvector e becomes max(l, -allowance/|g|^2),
// g_k = beta_k . e.  allowance = density/(4 hhat^2) makes the element's
// geometric part plus a quarter of its area's inertia positive semidefinite
// (the quarters over a vertex's triangles never exceed its lumped mass on the
// structured mesh); 0 clamps compression entirely; infinity keeps it all.
double MembraneElement(const Triangle& t, const V3* x, double c11, double c12, double c22, double c66,
	double* g, double* h, double allowance)
{
	const double a = t.dmInv[0], b = t.dmInv[1], c = t.dmInv[2], d = t.dmInv[3];
	const V3 d1 = Sub(x[1], x[0]), d2 = Sub(x[2], x[0]);
	const V3 f1 = Add(Mul(d1, a), Mul(d2, c)), f2 = Add(Mul(d1, b), Mul(d2, d));
	const double e1 = 0.5*(Dot(f1, f1) - 1), e2 = 0.5*(Dot(f2, f2) - 1), e3 = Dot(f1, f2);
	const double s1 = c11*e1 + c12*e2, s2 = c12*e1 + c22*e2, s3 = c66*e3;
	const double area = t.area;
	const double energy = 0.5*area*(e1*s1 + e2*s2 + e3*s3);
	if (!g && !h) return energy;
	const double beta[3][2] = {{-a - c, -b - d}, {a, b}, {c, d}};
	if (g) {
		for (int k = 0; k < 3; ++k) {
			const double p = s1*beta[k][0] + s3*beta[k][1], q = s3*beta[k][0] + s2*beta[k][1];
			for (int r = 0; r < 3; ++r) g[3*k + r] = area*(f1[r]*p + f2[r]*q);
		}
	}
	if (h) {
		// Positive part of the 2x2 second Piola-Kirchhoff stress for the geometric term.
		double p11 = s1, p22 = s2, p12 = s3;
		{
			const double mean = 0.5*(s1 + s2), half = 0.5*(s1 - s2);
			const double radius = std::sqrt(half*half + s3*s3);
			const double l1 = mean + radius, l2 = mean - radius;
			if (l2 < 0 && allowance < std::numeric_limits<double>::infinity()) {
				// Unit eigenvectors e1 (of l1) and e2 = perp(e1).
				double e1x = 1, e1y = 0;
				if (radius > 0) {
					const double ax = s3, ay = l1 - s1, bx = l1 - s2, by = s3;
					const double la = ax*ax + ay*ay, lb = bx*bx + by*by;
					if (la >= lb) { const double l = std::sqrt(la); e1x = ax/l; e1y = ay/l; }
					else { const double l = std::sqrt(lb); e1x = bx/l; e1y = by/l; }
				}
				const double e2x = -e1y, e2y = e1x;
				auto limit = [&](double l, double ex, double ey) {
					if (l >= 0) return l;
					double gg = 0;
					for (int k = 0; k < 3; ++k) { const double gk = beta[k][0]*ex + beta[k][1]*ey; gg += gk*gk; }
					return gg > 0 ? std::max(l, -allowance/gg) : 0.0;
				};
				const double m1 = limit(l1, e1x, e1y), m2 = limit(l2, e2x, e2y);
				p11 = m1*e1x*e1x + m2*e2x*e2x;
				p22 = m1*e1y*e1y + m2*e2y*e2y;
				p12 = m1*e1x*e1y + m2*e2x*e2y;
			}
		}
		V3 r[3][3]; // r[k][p]: Voigt row p of B_k
		V3 q[3][3]; // C r
		for (int k = 0; k < 3; ++k) {
			r[k][0] = Mul(f1, beta[k][0]);
			r[k][1] = Mul(f2, beta[k][1]);
			r[k][2] = Add(Mul(f2, beta[k][0]), Mul(f1, beta[k][1]));
			q[k][0] = Add(Mul(r[k][0], c11), Mul(r[k][1], c12));
			q[k][1] = Add(Mul(r[k][0], c12), Mul(r[k][1], c22));
			q[k][2] = Mul(r[k][2], c66);
		}
		for (int k = 0; k < 3; ++k) for (int l = 0; l < 3; ++l) {
			const double geo = area*(beta[k][0]*(p11*beta[l][0] + p12*beta[l][1]) +
				beta[k][1]*(p12*beta[l][0] + p22*beta[l][1]));
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) {
				double s = 0;
				for (int p = 0; p < 3; ++p) s += r[k][p][i]*q[l][p][j];
				h[(3*k + i)*9 + 3*l + j] = area*s + (i == j ? geo : 0.0);
			}
		}
	}
	return energy;
}

// Hinge: energy k theta^2, exact gradient, Gauss-Newton Hessian.
double HingeElement(const V3* x, double k, double* g, double* h)
{
	const V3 e = Sub(x[1], x[0]);
	const V3 nA = Cross(e, Sub(x[2], x[0])), nB = Cross(Sub(x[3], x[0]), e);
	const double le2 = Dot(e, e), lA2 = Dot(nA, nA), lB2 = Dot(nB, nB);
	if (!(le2 > 0) || !(lA2 > 0) || !(lB2 > 0)) {
		if (g) std::fill(g, g + 12, 0.0);
		if (h) std::fill(h, h + 144, 0.0);
		return 0;
	}
	const double le = std::sqrt(le2);
	const double theta = std::atan2(Dot(Cross(nA, nB), e)/le, Dot(nA, nB));
	const double energy = k*theta*theta;
	if (!g && !h) return energy;
	const V3 uA = Mul(nA, le/lA2), uB = Mul(nB, le/lB2); // unit normal / height
	const double sA = Dot(Sub(x[2], x[0]), e)/le2, sB = Dot(Sub(x[3], x[0]), e)/le2;
	V3 grad[4];
	grad[0] = Add(Mul(uA, 1 - sA), Mul(uB, 1 - sB));
	grad[1] = Add(Mul(uA, sA), Mul(uB, sB));
	grad[2] = Mul(uA, -1);
	grad[3] = Mul(uB, -1);
	if (g) for (int a = 0; a < 4; ++a) for (int r = 0; r < 3; ++r) g[3*a + r] = 2*k*theta*grad[a][r];
	if (h) for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
		for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
			h[(3*a + i)*12 + 3*b + j] = 2*k*grad[a][i]*grad[b][j];
	return energy;
}

// Discrete rod bending at the middle vertex: E = k |kappa b|^2 with
// kappa b = 2 e1 x e2 / (|e1||e2| + e1.e2); exact gradient, Gauss-Newton Hessian.
double RodElement(const V3* x, double k, double* g, double* h)
{
	const V3 e1 = Sub(x[1], x[0]), e2 = Sub(x[2], x[1]);
	const double l1 = std::sqrt(Dot(e1, e1)), l2 = std::sqrt(Dot(e2, e2));
	const double den = l1*l2 + Dot(e1, e2);
	if (!(l1 > 0) || !(l2 > 0) || !(den > 1e-12*l1*l2)) {
		if (g) std::fill(g, g + 9, 0.0);
		if (h) std::fill(h, h + 81, 0.0);
		return 0;
	}
	const V3 kb = Mul(Cross(e1, e2), 2/den);
	const double energy = k*Dot(kb, kb);
	if (!g && !h) return energy;
	// d kb / d e1 = (-2 [e2]x - kb (l2 e1/l1 + e2)^T)/den, d kb / d e2 = (2 [e1]x - kb (l1 e2/l2 + e1)^T)/den
	const V3 p1 = Add(Mul(e1, l2/l1), e2), p2 = Add(Mul(e2, l1/l2), e1);
	double d1[9], d2[9];
	const double c2[9] = {0, -e2[2], e2[1], e2[2], 0, -e2[0], -e2[1], e2[0], 0};
	const double c1[9] = {0, -e1[2], e1[1], e1[2], 0, -e1[0], -e1[1], e1[0], 0};
	for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
		d1[3*r + c] = (-2*c2[3*r + c] - kb[r]*p1[c])/den;
		d2[3*r + c] = (2*c1[3*r + c] - kb[r]*p2[c])/den;
	}
	double j[3][9]; // j[a]: d kb / d x_a, row-major 3x3
	for (int q = 0; q < 9; ++q) { j[0][q] = -d1[q]; j[1][q] = d1[q] - d2[q]; j[2][q] = d2[q]; }
	if (g) for (int a = 0; a < 3; ++a) for (int c = 0; c < 3; ++c)
		g[3*a + c] = 2*k*(j[a][c]*kb[0] + j[a][3 + c]*kb[1] + j[a][6 + c]*kb[2]);
	if (h) for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b)
		for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
			h[(3*a + r)*9 + 3*b + c] = 2*k*(j[a][r]*j[b][c] + j[a][3 + r]*j[b][3 + c] + j[a][6 + r]*j[b][6 + c]);
	return energy;
}

bool Invertible2(double m00, double m01, double m10, double m11, double* inv, double& det)
{
	det = m00*m11 - m01*m10;
	if (!(std::fabs(det) > 0)) return false;
	inv[0] = m11/det; inv[1] = -m01/det; inv[2] = -m10/det; inv[3] = m00/det;
	return true;
}
} // namespace

const char* TriangulationName(Triangulation t)
{
	return t == Triangulation::Alternating ? "alternating" : "consistent";
}
const char* BendingModelName(BendingModel b)
{
	return b == BendingModel::Lines ? "lines" : "hinges";
}
const char* SchemeName(Scheme s)
{
	switch (s) {
	case Scheme::BackwardEuler: return "backward_euler";
	case Scheme::BDF2: return "bdf2";
	case Scheme::ImplicitMidpoint: break;
	}
	return "implicit_midpoint";
}
const char* LinearSolverName(LinearSolver s)
{
	switch (s) {
	case LinearSolver::Auto: return "auto";
	case LinearSolver::ConjugateGradient: return "pcg";
	case LinearSolver::Direct: return "direct";
	case LinearSolver::MultigridCG: break;
	}
	return "mg_pcg";
}

V3 Motion::Position(const V3& rest, double t) const
{
	switch (type) {
	case Type::Fixed: return rest;
	case Type::Translate: return Add(rest, Mul(vector, t));
	case Type::Oscillate: return Add(rest, Mul(vector, std::sin(2*Pi*frequency*t)));
	case Type::Rotate: {
		const double angle = rate*t, c = std::cos(angle), s = std::sin(angle);
		const V3 p = Sub(rest, center);
		const V3 r = Add(Add(Mul(p, c), Mul(Cross(axis, p), s)), Mul(axis, Dot(axis, p)*(1 - c)));
		return Add(center, r);
	}
	}
	return rest;
}

V3 Motion::Velocity(const V3& rest, double t) const
{
	switch (type) {
	case Type::Fixed: return {{0, 0, 0}};
	case Type::Translate: return vector;
	case Type::Oscillate: return Mul(vector, 2*Pi*frequency*std::cos(2*Pi*frequency*t));
	case Type::Rotate: {
		const V3 p = Sub(Position(rest, t), center);
		return Mul(Cross(axis, p), rate);
	}
	}
	return {{0, 0, 0}};
}

V3 Motion::Advance(const V3& x, double t0, double t1) const
{
	switch (type) {
	case Type::Fixed: return x;
	case Type::Translate: return Add(x, Mul(vector, t1 - t0));
	case Type::Oscillate:
		return Add(x, Mul(vector, std::sin(2*Pi*frequency*t1) - std::sin(2*Pi*frequency*t0)));
	case Type::Rotate: {
		const double angle = rate*(t1 - t0), c = std::cos(angle), s = std::sin(angle);
		const V3 p = Sub(x, center);
		return Add(center, Add(Add(Mul(p, c), Mul(Cross(axis, p), s)), Mul(axis, Dot(axis, p)*(1 - c))));
	}
	}
	return x;
}

const char* ContactModelName(ContactModel c)
{
	return c == ContactModel::Penalty ? "penalty" : "barrier";
}

double Collider::Distance(const V3& x, V3& gradient, double* hessian) const
{
	if (hessian) std::fill(hessian, hessian + 9, 0.0);
	switch (type) {
	case Type::Plane:
		gradient = b;
		return Dot(Sub(x, a), b);
	case Type::Sphere: {
		const V3 d = Sub(x, a);
		const double l = std::sqrt(Dot(d, d));
		gradient = l > 0 ? Mul(d, 1/l) : V3{{0, 1, 0}};
		if (hessian && l > 0)
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
				hessian[3*i + j] = ((i == j ? 1.0 : 0.0) - gradient[i]*gradient[j])/l;
		return l - radius;
	}
	case Type::Cylinder: {
		const V3 w = Sub(x, a);
		const V3 perp = Sub(w, Mul(b, Dot(w, b)));
		const double l = std::sqrt(Dot(perp, perp));
		V3 n{{0, 1, 0}};
		if (l > 0) n = Mul(perp, 1/l);
		gradient = n;
		if (hessian && l > 0)
			for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
				hessian[3*i + j] = ((i == j ? 1.0 : 0.0) - b[i]*b[j] - n[i]*n[j])/l;
		return l - radius;
	}
	case Type::Box: {
		V3 out{{0, 0, 0}};
		bool outside = false, clamped[3] = {false, false, false};
		for (int k = 0; k < 3; ++k) {
			if (x[k] < a[k]) { out[k] = x[k] - a[k]; outside = clamped[k] = true; }
			else if (x[k] > b[k]) { out[k] = x[k] - b[k]; outside = clamped[k] = true; }
		}
		if (outside) {
			const double l = std::sqrt(Dot(out, out));
			gradient = Mul(out, 1/l);
			if (hessian)
				for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
					hessian[3*i + j] = ((i == j && clamped[i] ? 1.0 : 0.0) - gradient[i]*gradient[j])/l;
			return l;
		}
		double best = x[0] - a[0];
		gradient = {{-1, 0, 0}};
		for (int k = 0; k < 3; ++k) {
			if (x[k] - a[k] < best) { best = x[k] - a[k]; gradient = {{0, 0, 0}}; gradient[k] = -1; }
			if (b[k] - x[k] < best) { best = b[k] - x[k]; gradient = {{0, 0, 0}}; gradient[k] = 1; }
		}
		return -best;
	}
	}
	gradient = {{0, 1, 0}};
	return 0;
}

bool BuildSheet(const SheetSpec& spec, const Material& material, Sheet& sheet, std::string& error)
{
	if (spec.verticesU < 2 || spec.verticesV < 2 || !(spec.sizeU > 0) || !(spec.sizeV > 0) ||
		!(spec.stretchU > 0) || !(spec.stretchV > 0)) {
		error = "sheet needs at least 2 x 2 vertices, a positive size and a positive stretch";
		return false;
	}
	if (static_cast<std::uint64_t>(spec.verticesU)*spec.verticesV > 4000000u) {
		error = "sheet has too many vertices";
		return false;
	}
	if (std::fabs(Dot(spec.axisU, spec.axisU) - 1) > 1e-9 || std::fabs(Dot(spec.axisV, spec.axisV) - 1) > 1e-9 ||
		std::fabs(Dot(spec.axisU, spec.axisV)) > 1e-9) {
		error = "sheet axes must be orthonormal";
		return false;
	}
	Sheet s;
	s.spec = spec;
	const std::uint32_t nu = spec.verticesU, nv = spec.verticesV;
	s.hu = spec.sizeU/static_cast<double>(nu - 1);
	s.hv = spec.sizeV/static_cast<double>(nv - 1);
	const std::uint32_t n = nu*nv;
	s.materialU.resize(n); s.materialV.resize(n); s.mass.resize(n); s.area.resize(n); s.initial.resize(3*static_cast<std::size_t>(n));
	for (std::uint32_t j = 0; j < nv; ++j) for (std::uint32_t i = 0; i < nu; ++i) {
		const std::uint32_t q = s.Index(i, j);
		const double u = static_cast<double>(i)*s.hu, v = static_cast<double>(j)*s.hv;
		s.materialU[q] = u; s.materialV[q] = v;
		const double wu = (i == 0 || i == nu - 1) ? 0.5*s.hu : s.hu;
		const double wv = (j == 0 || j == nv - 1) ? 0.5*s.hv : s.hv;
		s.area[q] = wu*wv;
		s.mass[q] = material.density*wu*wv;
		Put(s.initial, q, Add(spec.origin, Add(Mul(spec.axisU, spec.stretchU*u), Mul(spec.axisV, spec.stretchV*v))));
	}
	// Triangles, counter-clockwise in (u, v).
	for (std::uint32_t j = 0; j + 1 < nv; ++j) for (std::uint32_t i = 0; i + 1 < nu; ++i) {
		const std::uint32_t a = s.Index(i, j), b = s.Index(i + 1, j), c = s.Index(i + 1, j + 1), d = s.Index(i, j + 1);
		const bool slash = spec.triangulation == Triangulation::Consistent || ((i + j) % 2 == 0);
		const std::uint32_t tris[2][3] = {{a, b, slash ? c : d}, {slash ? a : b, c, d}};
		for (const auto& tv : tris) {
			Triangle t;
			for (int k = 0; k < 3; ++k) t.v[k] = tv[k];
			const double m00 = s.materialU[t.v[1]] - s.materialU[t.v[0]], m01 = s.materialU[t.v[2]] - s.materialU[t.v[0]];
			const double m10 = s.materialV[t.v[1]] - s.materialV[t.v[0]], m11 = s.materialV[t.v[2]] - s.materialV[t.v[0]];
			double det = 0;
			if (!Invertible2(m00, m01, m10, m11, t.dmInv, det) || det <= 0) { error = "degenerate sheet triangle"; return false; }
			t.area = 0.5*det;
			s.triangles.push_back(t);
		}
	}
	// Hinges on interior edges, in edge-key order.
	if (material.bendWarp > 0) {
		struct Side { std::uint32_t p, q, opposite; };
		std::map<std::uint64_t, std::vector<Side>> edges;
		for (const auto& t : s.triangles)
			for (int k = 0; k < 3; ++k) {
				const std::uint32_t p = t.v[k], q = t.v[(k + 1) % 3], o = t.v[(k + 2) % 3];
				const std::uint64_t key = (static_cast<std::uint64_t>(std::min(p, q)) << 32) | std::max(p, q);
				edges[key].push_back({p, q, o});
			}
		const double kWarp = 0.5*material.bendWarp*s.hv/s.hu;
		const double kWeft = 0.5*material.bendWeft*s.hu/s.hv;
		const double kDiag = std::sqrt(material.bendWarp*material.bendWeft)*s.hu*s.hv/(s.hu*s.hu + s.hv*s.hv);
		for (const auto& e : edges) {
			if (e.second.size() != 2) continue;
			const Side& A = e.second[0];
			const Side& B = e.second[1];
			if (B.p != A.q || B.q != A.p) { error = "inconsistent sheet orientation"; return false; }
			Hinge hinge;
			hinge.v[0] = A.p; hinge.v[1] = A.q; hinge.v[2] = A.opposite; hinge.v[3] = B.opposite;
			const std::uint32_t ip = A.p % nu, jp = A.p / nu, iq = A.q % nu, jq = A.q / nu;
			if (jp == jq) { hinge.kind = HingeKind::Weft; hinge.stiffness = kWeft; }       // edge along u
			else if (ip == iq) { hinge.kind = HingeKind::Warp; hinge.stiffness = kWarp; }  // edge along v
			else { hinge.kind = HingeKind::Diagonal; hinge.stiffness = kDiag; }
			if (spec.bending == BendingModel::Hinges || hinge.kind == HingeKind::Diagonal) s.hinges.push_back(hinge);
		}
		if (spec.bending == BendingModel::Lines) {
			// Rows (warp lines) then columns (weft lines), each interior vertex once.
			for (std::uint32_t j = 0; j < nv; ++j) for (std::uint32_t i = 1; i + 1 < nu; ++i) {
				const double share = (j == 0 || j == nv - 1) ? 0.5*s.hv : s.hv;
				Rod r;
				r.v[0] = s.Index(i - 1, j); r.v[1] = s.Index(i, j); r.v[2] = s.Index(i + 1, j);
				r.kind = HingeKind::Warp;
				r.stiffness = 0.5*material.bendWarp*share/s.hu;
				s.rods.push_back(r);
			}
			for (std::uint32_t i = 0; i < nu; ++i) for (std::uint32_t j = 1; j + 1 < nv; ++j) {
				const double share = (i == 0 || i == nu - 1) ? 0.5*s.hu : s.hu;
				Rod r;
				r.v[0] = s.Index(i, j - 1); r.v[1] = s.Index(i, j); r.v[2] = s.Index(i, j + 1);
				r.kind = HingeKind::Weft;
				r.stiffness = 0.5*material.bendWeft*share/s.hv;
				s.rods.push_back(r);
			}
		}
	}
	sheet = std::move(s);
	return true;
}

// ------------------------------------------------------------------ system

bool ClothSystem::Initialize(const Sheet& sheet, const ModelConfig& config, std::string& error)
{
	if (!ValidateMaterial(config.material, error)) return false;
	sheet_ = sheet;
	config_ = config;
	const Material& m = config.material;
	const double nu12 = m.poisson, nu21 = m.poisson*m.stretchWeft/m.stretchWarp, den = 1 - nu12*nu21;
	c11_ = m.stretchWarp/den; c22_ = m.stretchWeft/den; c12_ = nu12*m.stretchWeft/den; c66_ = m.shear;
	const std::uint32_t n = sheet.Vertices();
	fixed_.assign(n, 0);
	pinGroup_.assign(n, -1);
	for (std::size_t g = 0; g < config.pins.size(); ++g)
		for (std::uint32_t v : config.pins[g].vertices) {
			if (v >= n) { error = "pinned vertex out of range"; return false; }
			fixed_[v] = 1;
			pinGroup_[v] = static_cast<int>(g);
		}
	std::vector<std::pair<std::uint32_t,std::uint32_t>> pairs;
	for (const auto& t : sheet.triangles)
		for (int a = 0; a < 3; ++a) for (int b = a + 1; b < 3; ++b) pairs.emplace_back(t.v[a], t.v[b]);
	for (const auto& h : sheet.hinges)
		for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b) pairs.emplace_back(h.v[a], h.v[b]);
	for (const auto& r : sheet.rods)
		for (int a = 0; a < 3; ++a) for (int b = a + 1; b < 3; ++b) pairs.emplace_back(r.v[a], r.v[b]);
	pattern_.Build(n, pairs);
	triSlots_.resize(sheet.triangles.size());
	for (std::size_t t = 0; t < sheet.triangles.size(); ++t)
		for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b)
			triSlots_[t][3*a + b] = pattern_.Slot(sheet.triangles[t].v[a], sheet.triangles[t].v[b]);
	hingeSlots_.resize(sheet.hinges.size());
	for (std::size_t e = 0; e < sheet.hinges.size(); ++e)
		for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
			hingeSlots_[e][4*a + b] = pattern_.Slot(sheet.hinges[e].v[a], sheet.hinges[e].v[b]);
	rodSlots_.resize(sheet.rods.size());
	for (std::size_t e = 0; e < sheet.rods.size(); ++e)
		for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b)
			rodSlots_[e][3*a + b] = pattern_.Slot(sheet.rods[e].v[a], sheet.rods[e].v[b]);
	// Clamp correction: a bending element with one wing entirely pinned and the
	// other free represents curvature over half a cell.
	hingeScale_.assign(sheet.hinges.size(), 1.0);
	for (std::size_t e = 0; e < sheet.hinges.size(); ++e) {
		const auto& h = sheet.hinges[e].v;
		const bool edge = fixed_[h[0]] && fixed_[h[1]];
		const bool a = edge && fixed_[h[2]], b = edge && fixed_[h[3]];
		if (a != b) hingeScale_[e] = 2.0;
	}
	rodScale_.assign(sheet.rods.size(), 1.0);
	for (std::size_t e = 0; e < sheet.rods.size(); ++e) {
		const auto& r = sheet.rods[e].v;
		const bool a = fixed_[r[0]] && fixed_[r[1]], b = fixed_[r[1]] && fixed_[r[2]];
		if (a != b) rodScale_[e] = 2.0;
	}
	// Banded ordering: u-fastest or v-fastest, whichever is narrower.
	const std::uint32_t nu = sheet.spec.verticesU, nv = sheet.spec.verticesV;
	std::vector<std::uint32_t> uFast(n), vFast(n);
	for (std::uint32_t k = 0; k < n; ++k) uFast[k] = k;
	for (std::uint32_t i = 0, k = 0; i < nu; ++i) for (std::uint32_t j = 0; j < nv; ++j) vFast[k++] = sheet.Index(i, j);
	const std::size_t bu = BandedCholesky::Bandwidth(pattern_, uFast), bv = BandedCholesky::Bandwidth(pattern_, vFast);
	order_ = bu <= bv ? uFast : vFast;
	const double bw = static_cast<double>(std::min(bu, bv));
	directCost_ = 3.0*static_cast<double>(n)*bw*bw;
	barrier_.reset();
	if (config.contact == ContactModel::Barrier) {
		barrier_ = std::make_shared<BarrierContact>();
		if (!barrier_->Initialize(sheet, config, error)) return false;
		if (!barrier_->Active()) barrier_.reset();
	} else if (!config.meshColliders.empty()) {
		error = "mesh colliders need barrier contact";
		return false;
	}
	damping_ = pattern_;
	multigrid_.Build(pattern_, nu, nv, fixed_);
	multigrid_.SetPool(pool_.get());
	// Greedy colouring of each element family in element order.
	auto colour = [&](std::size_t count, int arity, auto vertexOf, std::vector<std::vector<std::uint32_t>>& colours) {
		std::vector<std::uint64_t> used(n, 0);
		colours.clear();
		for (std::size_t e = 0; e < count; ++e) {
			std::uint64_t taken = 0;
			for (int a = 0; a < arity; ++a) taken |= used[vertexOf(e, a)];
			int c = 0;
			while (c < 63 && (taken >> c & 1u)) ++c;
			for (int a = 0; a < arity; ++a) used[vertexOf(e, a)] |= std::uint64_t(1) << c;
			if (colours.size() <= static_cast<std::size_t>(c)) colours.resize(static_cast<std::size_t>(c) + 1);
			colours[static_cast<std::size_t>(c)].push_back(static_cast<std::uint32_t>(e));
		}
	};
	colour(sheet.triangles.size(), 3, [&](std::size_t e, int a) { return sheet_.triangles[e].v[a]; }, triColors_);
	colour(sheet.hinges.size(), 4, [&](std::size_t e, int a) { return sheet_.hinges[e].v[a]; }, hingeColors_);
	colour(sheet.rods.size(), 3, [&](std::size_t e, int a) { return sheet_.rods[e].v[a]; }, rodColors_);
	elementEnergy_.assign(sheet.triangles.size() + sheet.hinges.size() + sheet.rods.size(), 0.0);
	Reset(0);
	return true;
}

void ClothSystem::SetThreads(unsigned threads)
{
	pool_ = threads > 1 ? std::make_shared<ParallelPool>(threads) : nullptr;
	multigrid_.SetPool(pool_.get());
}

void ClothSystem::PinTargets(double t, std::vector<double>& pos, std::vector<double>* vel) const
{
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
		if (pinGroup_[q] < 0) continue;
		const Motion& motion = config_.pins[static_cast<std::size_t>(pinGroup_[q])].motion;
		const V3 rest = At(sheet_.initial, q);
		Put(pos, q, motion.Position(rest, t));
		if (vel) Put(*vel, q, motion.Velocity(rest, t));
	}
}

void ClothSystem::Reset(double t)
{
	x = sheet_.initial;
	v.assign(x.size(), 0.0);
	PinTargets(t, x, &v);
	xPrev.clear(); vPrev.clear();
	hPrev = 0; havePrev = false; time = t;
}

void ClothSystem::MaskFixed(BlockMatrix& h) const
{
	const auto& rs = h.RowStart();
	const auto& col = h.Column();
	for (std::uint32_t i = 0; i < h.Blocks(); ++i)
		for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
			const std::uint32_t j = col[k];
			if (!fixed_[i] && !fixed_[j]) continue;
			double* b = h.Block(k);
			for (int r = 0; r < 9; ++r) b[r] = 0;
			if (i == j) b[0] = b[4] = b[8] = 1;
		}
}

double ClothSystem::ElementEnergies(const std::vector<double>& pos, std::vector<double>* gradient,
	BlockMatrix* hessian, bool contact, double allowance) const
{
	// Each family runs colour by colour; the elements of a colour share no vertex,
	// so they scatter into the gradient and Hessian without conflicts (in chunks
	// that may run in parallel).  Energies go to a per-element buffer summed in
	// element order afterwards.  The result does not depend on the pool.
	ParallelPool* pool = pool_.get();
	constexpr std::size_t span = 128;
	const std::size_t nt = sheet_.triangles.size(), nh = sheet_.hinges.size();
	double* energies = elementEnergy_.data();
	for (const auto& list : triColors_)
		ParallelFor(pool, Chunks(list.size(), span), [&](std::size_t chunk) {
			double ge[9], he[81];
			const std::size_t end = std::min(list.size(), (chunk + 1)*span);
			for (std::size_t k = chunk*span; k < end; ++k) {
				const std::uint32_t t = list[k];
				const Triangle& tri = sheet_.triangles[t];
				const V3 xs[3] = {At(pos, tri.v[0]), At(pos, tri.v[1]), At(pos, tri.v[2])};
				energies[t] = MembraneElement(tri, xs, c11_, c12_, c22_, c66_, gradient ? ge : nullptr, hessian ? he : nullptr, allowance);
				if (gradient) for (int a = 0; a < 3; ++a) Accumulate(*gradient, tri.v[a], {{ge[3*a], ge[3*a+1], ge[3*a+2]}});
				if (hessian) for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
					double* blk = hessian->Block(triSlots_[t][3*a + b]);
					for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] += he[(3*a + i)*9 + 3*b + j];
				}
			}
		});
	for (const auto& list : hingeColors_)
		ParallelFor(pool, Chunks(list.size(), span), [&](std::size_t chunk) {
			double ge[12], he[144];
			const std::size_t end = std::min(list.size(), (chunk + 1)*span);
			for (std::size_t k = chunk*span; k < end; ++k) {
				const std::uint32_t e = list[k];
				const Hinge& hg = sheet_.hinges[e];
				const V3 xs[4] = {At(pos, hg.v[0]), At(pos, hg.v[1]), At(pos, hg.v[2]), At(pos, hg.v[3])};
				energies[nt + e] = HingeElement(xs, hg.stiffness*hingeScale_[e], gradient ? ge : nullptr, hessian ? he : nullptr);
				if (gradient) for (int a = 0; a < 4; ++a) Accumulate(*gradient, hg.v[a], {{ge[3*a], ge[3*a+1], ge[3*a+2]}});
				if (hessian) for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) {
					double* blk = hessian->Block(hingeSlots_[e][4*a + b]);
					for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] += he[(3*a + i)*12 + 3*b + j];
				}
			}
		});
	for (const auto& list : rodColors_)
		ParallelFor(pool, Chunks(list.size(), span), [&](std::size_t chunk) {
			double ge[9], he[81];
			const std::size_t end = std::min(list.size(), (chunk + 1)*span);
			for (std::size_t k = chunk*span; k < end; ++k) {
				const std::uint32_t e = list[k];
				const Rod& rd = sheet_.rods[e];
				const V3 xs[3] = {At(pos, rd.v[0]), At(pos, rd.v[1]), At(pos, rd.v[2])};
				energies[nt + nh + e] = RodElement(xs, rd.stiffness*rodScale_[e], gradient ? ge : nullptr, hessian ? he : nullptr);
				if (gradient) for (int a = 0; a < 3; ++a) Accumulate(*gradient, rd.v[a], {{ge[3*a], ge[3*a+1], ge[3*a+2]}});
				if (hessian) for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) {
					double* blk = hessian->Block(rodSlots_[e][3*a + b]);
					for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] += he[(3*a + i)*9 + 3*b + j];
				}
			}
		});
	double energy = 0;
	for (double e : elementEnergy_) energy += e;
	if (contact && config_.contact == ContactModel::Penalty && !config_.colliders.empty()) {
		for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
			const V3 p = At(pos, q);
			const double k = config_.contactStiffness*sheet_.area[q];
			for (const auto& c : config_.colliders) {
				V3 n;
				// C2 penalty in the depth d: k d^3/(6 delta) for d < delta, then
				// k (d^2/2 - delta d/2 + delta^2/6) (a quadratic penalty's Hessian jump at
				// d = 0 makes Newton chatter on the contact set).
				const double d = config_.contactThickness - c.Distance(p, n);
				if (!(d > 0)) continue;
				const double delta = std::max(0.5*config_.contactThickness, 1e-5);
				double e, f, s;
				if (d < delta) { e = k*d*d*d/(6*delta); f = k*d*d/(2*delta); s = k*d/delta; }
				else { e = k*(0.5*d*d - 0.5*delta*d + delta*delta/6); f = k*(d - 0.5*delta); s = k; }
				energy += e;
				if (gradient) Accumulate(*gradient, q, Mul(n, -f));
				if (hessian) {
					double* blk = hessian->Block(pattern_.DiagonalSlot(q));
					for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) blk[3*i + j] += s*n[i]*n[j];
				}
			}
		}
	}
	return energy;
}

double ClothSystem::Evaluate(const Potential& p, const std::vector<double>& pos, std::vector<double>* gradient,
	BlockMatrix* hessian, bool project) const
{
	const std::size_t dofs = pos.size();
	if (gradient) gradient->assign(dofs, 0.0);
	if (hessian) hessian->Zero();
	// Projected: keep the compression the inertia can absorb (clamp it in statics).
	const double allowance = project ? 0.25*config_.material.density*p.inertia : std::numeric_limits<double>::infinity();
	double energy = ElementEnergies(pos, gradient, hessian, true, allowance);
	if (p.contact && barrier_) {
		const double e = barrier_->Evaluate(*p.contact, pos, gradient, hessian, project, fixed_);
		if (!std::isfinite(e)) return std::numeric_limits<double>::infinity();
		energy += e;
	}
	if (p.inertia > 0) {
		double s = 0;
		for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
			const double w = p.inertia*sheet_.mass[q];
			for (int r = 0; r < 3; ++r) {
				const std::size_t k = 3*static_cast<std::size_t>(q) + r;
				const double d = pos[k] - p.xHat[k];
				s += w*d*d;
				if (gradient) (*gradient)[k] += w*d;
			}
			if (hessian) { double* b = hessian->Block(hessian->DiagonalSlot(q)); b[0] += w; b[4] += w; b[8] += w; }
		}
		energy += 0.5*s;
	}
	double work = 0;
	for (std::size_t k = 0; k < dofs; ++k) {
		work += p.force[k]*(pos[k] - p.xRef[k]);
		if (gradient) (*gradient)[k] -= p.force[k];
	}
	energy -= work;
	if (p.useDamping) {
		std::vector<double> r(dofs), dr;
		for (std::size_t k = 0; k < dofs; ++k) r[k] = pos[k] - p.xTilde[k];
		damping_.Multiply(r, dr, pool_.get());
		double s = 0;
		for (std::size_t k = 0; k < dofs; ++k) {
			s += r[k]*dr[k];
			if (gradient) (*gradient)[k] += p.dampingScale*dr[k];
		}
		energy += 0.5*p.dampingScale*s;
		if (hessian) hessian->AddScaled(damping_, p.dampingScale);
	}
	if (gradient)
		for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q)
			if (fixed_[q]) for (int r = 0; r < 3; ++r) (*gradient)[3*static_cast<std::size_t>(q) + r] = 0;
	if (hessian) MaskFixed(*hessian);
	return energy;
}

bool ClothSystem::Solve(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& solution,
	const NewtonOptions& options, bool preferDirect, StepStats& stats, bool* indefinite, bool reuse) const
{
	if (indefinite) *indefinite = false;
	const bool direct = options.solver == LinearSolver::Direct ||
		(options.solver == LinearSolver::Auto && directCost_ <= (preferDirect ? 5e9 : 2e7));
	if (direct) {
		if (!(reuse && factorValid_)) factorValid_ = factor_.Factor(a, order_);
		if (factorValid_) {
			factor_.Solve(b, solution);
			++stats.directSolves;
			return true;
		}
		if (indefinite) { *indefinite = true; solution.assign(b.size(), 0.0); return false; }
	}
	LinearSolveStats ls;
	bool ok = false;
	if (options.solver == LinearSolver::ConjugateGradient)
		ok = SolveConjugateGradient(a, b, solution, options.cgTolerance, options.cgIterations, ls);
	else if (!reuse && !multigrid_.Setup(a)) {
		// The coarsest Galerkin operator (or a line block) is not positive definite: so is not A.
		if (indefinite) *indefinite = true;
		solution.assign(b.size(), 0.0);
		return false;
	} else ok = SolveConjugateGradient(a, b, solution, options.cgTolerance, options.cgIterations, ls, multigrid_);
	stats.cgIterations += ls.iterations;
	if (!ok && indefinite) *indefinite = true;
	return ok || ls.iterations > 0;
}

bool ClothSystem::Newton(const Potential& p, std::vector<double>& pos, const NewtonOptions& options, bool preferDirect,
	StepStats& stats, std::string& error) const
{
	const std::uint32_t n = sheet_.Vertices();
	const std::size_t dofs = pos.size();
	std::vector<double> g, gt, rhs(dofs), dx, trial(dofs);
	BlockMatrix h = pattern_;
	auto residual = [&](const std::vector<double>& grad) {
		double worst = 0;
		for (std::uint32_t q = 0; q < n; ++q) {
			if (fixed_[q]) continue;
			const std::size_t k = 3*static_cast<std::size_t>(q);
			const double r = std::sqrt(grad[k]*grad[k] + grad[k+1]*grad[k+1] + grad[k+2]*grad[k+2])/sheet_.mass[q];
			worst = std::max(worst, r);
		}
		return worst;
	};
	// Converged when every free vertex's residual acceleration is within the
	// tolerance or within the floating-point floor of its own force: positions
	// carry about eps*|x| absolute error, so forces carry eps*|x|*|H_ii|
	// (this floor grows as 1/h^2 under refinement and is reported as such).
	// Without a current Hessian only the tolerance applies.
	auto converged = [&](const std::vector<double>& grad, const BlockMatrix* hess) {
		double scale = 0;
		for (double c : pos) scale = std::max(scale, std::fabs(c));
		const double floorFactor = 8*std::numeric_limits<double>::epsilon()*scale;
		for (std::uint32_t q = 0; q < n; ++q) {
			if (fixed_[q]) continue;
			const std::size_t k = 3*static_cast<std::size_t>(q);
			double limit = options.tolerance;
			if (hess) {
				const double* b = hess->Block(hess->DiagonalSlot(q));
				double hn = 0;
				for (int r = 0; r < 9; ++r) hn += b[r]*b[r];
				limit = std::max(limit, floorFactor*std::sqrt(hn)/sheet_.mass[q]);
			}
			const double r = std::sqrt(grad[k]*grad[k] + grad[k+1]*grad[k+1] + grad[k+2]*grad[k+2])/sheet_.mass[q];
			if (!(r <= limit)) return false;
		}
		return true;
	};
	// Each iteration tries the exact Hessian first (the inertia term usually keeps
	// it positive definite) and uses the projected one when the exact one is
	// found indefinite or gives no descent direction.  With lagHessian the first
	// iteration's matrix (and its factor or preconditioner) serves the whole solve.
	const bool exactFirst = options.exactHessianFirst;
	const bool lag = options.lagHessian;
	if (p.contact) barrier_->Candidates(*p.contact, pos, nullptr);
	double phi = Evaluate(p, pos, &g, &h, !exactFirst);
	bool haveH = true;   // h is the Hessian at pos (or the lagged one)
	bool hFresh = true;  // h changed since the last linear solve
	for (int it = 0;; ++it) {
		const double res = residual(g);
		stats.residual = res;
		if (!std::isfinite(res) || !std::isfinite(phi)) { error = "cloth Newton produced a nonfinite state"; return false; }
		if (converged(g, haveH ? &h : nullptr)) { ++stats.solves; stats.residualSum += res; return true; }
		if (it >= options.maxIterations) {
			if (options.requireConvergence) {
				error = "cloth Newton did not reach its tolerance (residual " + std::to_string(res) + " m/s^2)";
				return false;
			}
			++stats.unconverged;
			++stats.solves; stats.residualSum += res;
			return true;
		}
		if (!haveH) { Evaluate(p, pos, nullptr, &h, !exactFirst); haveH = hFresh = true; }
		++stats.newtonIterations;
		for (std::size_t k = 0; k < dofs; ++k) rhs[k] = -g[k];
		bool indefinite = false;
		Solve(h, rhs, dx, options, preferDirect, stats, &indefinite, !hFresh);
		hFresh = false;
		double slope = 0;
		for (std::size_t k = 0; k < dofs; ++k) slope += g[k]*dx[k];
		if (exactFirst && (indefinite || !(slope < 0) || !std::isfinite(slope))) {
			++stats.projections;
			Evaluate(p, pos, nullptr, &h, true);
			Solve(h, rhs, dx, options, preferDirect, stats, nullptr, false);
			slope = 0;
			for (std::size_t k = 0; k < dofs; ++k) slope += g[k]*dx[k];
		}
		if (!(slope < 0) || !std::isfinite(slope)) {
			// Fall back to a diagonally scaled gradient step.
			slope = 0;
			for (std::uint32_t q = 0; q < n; ++q) {
				const double* b = h.Block(h.DiagonalSlot(q));
				for (int r = 0; r < 3; ++r) {
					const std::size_t k = 3*static_cast<std::size_t>(q) + r;
					const double d = b[4*r] > 0 ? b[4*r] : 1.0;
					dx[k] = -g[k]/d;
					slope += g[k]*dx[k];
				}
			}
			if (!(slope < 0)) { error = "cloth Newton found no descent direction"; return false; }
		}
		// The full step is evaluated with its gradient (and, unless the matrix is
		// lagged or this is the last iteration of a truncated solve, its Hessian):
		// it is usually accepted.  Backtracked trials are evaluated energy-only.
		const bool lastTruncated = !options.requireConvergence && it + 1 >= options.maxIterations;
		const bool wantH = !lag && !lastTruncated;
		// Roundoff allowance: the potential's terms are O(|phi| + |slope|) and lose
		// about 1e-13 relative; within that band a step that lowers the residual is taken.
		const double slack = 1e-12*(std::fabs(phi) + std::fabs(slope));
		// Barrier contact: candidates for the whole segment, and CCD bounds the step.
		double alpha = 1;
		if (p.contact) {
			barrier_->Candidates(*p.contact, pos, &dx);
			alpha = barrier_->MaxStep(*p.contact, pos, dx);
			if (alpha < 1) ++stats.ccdLimited;
			if (!(alpha > 0)) { error = "cloth contact: the collision detection allows no step"; return false; }
		}
		bool accepted = false;
		for (int ls = 0; ls < 30 && !accepted; ++ls, alpha *= 0.5) {
			for (std::size_t k = 0; k < dofs; ++k) trial[k] = pos[k] + alpha*dx[k];
			const bool full = ls == 0;
			if (full && wantH) { haveH = false; hFresh = true; }
			const double phiTrial = full ? Evaluate(p, trial, &gt, wantH ? &h : nullptr, !exactFirst) :
				Evaluate(p, trial, nullptr, nullptr, true);
			if (!std::isfinite(phiTrial)) continue;
			bool haveGt = full;
			if (phiTrial <= phi + 1e-4*alpha*slope) accepted = true;
			else if (phiTrial <= phi + slack) {
				if (!haveGt) { Evaluate(p, trial, &gt, nullptr, true); haveGt = true; }
				accepted = residual(gt) < res;
			}
			if (accepted) {
				pos.swap(trial);
				phi = phiTrial;
				if (full) { g.swap(gt); haveH = wantH || lag; }
				else {
					const bool evalH = wantH;
					phi = Evaluate(p, pos, &g, evalH ? &h : nullptr, !exactFirst);
					haveH = evalH || lag;
					if (evalH) hFresh = true;
				}
			}
		}
		if (!accepted) { error = "cloth Newton line search failed"; return false; }
	}
}

void ClothSystem::ExternalForce(const std::vector<double>& pos, const std::vector<double>& vel, std::vector<double>& f) const
{
	const std::uint32_t n = sheet_.Vertices();
	f.assign(3*static_cast<std::size_t>(n), 0.0);
	for (std::uint32_t q = 0; q < n; ++q) Put(f, q, Mul(config_.gravity, sheet_.mass[q]));
	const bool pressure = config_.pressure != 0 || !portPressure.empty();
	const bool drag = config_.dragCoefficient > 0;
	if (pressure || drag) {
		std::vector<double> area(f.size(), 0.0); // vector area per vertex (current)
		for (const auto& t : sheet_.triangles) {
			const V3 x0 = At(pos, t.v[0]);
			const V3 nrm = Mul(Cross(Sub(At(pos, t.v[1]), x0), Sub(At(pos, t.v[2]), x0)), 1.0/6.0);
			for (int a = 0; a < 3; ++a) Accumulate(area, t.v[a], nrm);
		}
		for (std::uint32_t q = 0; q < n; ++q) {
			const V3 an = At(area, q);
			if (pressure) {
				const double pq = config_.pressure + (portPressure.empty() ? 0.0 : portPressure[q]);
				Accumulate(f, q, Mul(an, -pq));
			}
			if (drag) {
				const double la = std::sqrt(Dot(an, an));
				if (la > 0) {
					const V3 nh = Mul(an, 1/la);
					const double vn = Dot(At(vel, q), nh);
					Accumulate(f, q, Mul(nh, -0.5*config_.airDensity*config_.dragCoefficient*la*vn*std::fabs(vn)));
				}
			}
		}
	}
	if (!portTraction.empty())
		for (std::uint32_t q = 0; q < n; ++q) Accumulate(f, q, Mul(At(portTraction, q), sheet_.area[q]));
}

bool ClothSystem::StepOnce(double h, Scheme scheme, const NewtonOptions& options, StepStats& stats, std::string& error)
{
	const std::size_t dofs = x.size();
	const double tNext = time + h;
	Potential p;
	p.xRef = x;
	ExternalForce(x, v, p.force);
	const bool bdf2 = scheme == Scheme::BDF2 && havePrev;
	double a = 1/h, b = -1/h, c = 0;
	if (bdf2) {
		const double k = hPrev;
		a = 1/h + 1/(h + k); b = -(h + k)/(h*k); c = h/(k*(h + k));
	}
	p.xHat.resize(dofs);
	p.xTilde.resize(dofs);
	std::vector<double> guess(dofs);
	if (scheme == Scheme::ImplicitMidpoint) {
		p.inertia = 4/(h*h);
		for (std::size_t k = 0; k < dofs; ++k) {
			p.xHat[k] = x[k] + 0.5*h*v[k];
			p.xTilde[k] = x[k];
			guess[k] = p.xHat[k];
		}
		p.dampingScale = 2/h;
	} else {
		p.inertia = a*a;
		for (std::size_t k = 0; k < dofs; ++k) {
			const double cx = bdf2 ? b*x[k] + c*xPrev[k] : b*x[k];
			const double cv = bdf2 ? b*v[k] + c*vPrev[k] : b*v[k];
			p.xTilde[k] = -cx/a;
			p.xHat[k] = -cx/a - cv/(a*a);
			guess[k] = x[k] + h*v[k];
		}
		p.dampingScale = a;
	}
	std::vector<double> target(dofs), targetV(dofs);
	PinTargets(tNext, target, &targetV);
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
		if (!fixed_[q]) continue;
		for (int r = 0; r < 3; ++r) {
			const std::size_t k = 3*static_cast<std::size_t>(q) + r;
			guess[k] = scheme == Scheme::ImplicitMidpoint ? 0.5*(x[k] + target[k]) : target[k];
		}
	}
	const Material& m = config_.material;
	p.useDamping = config_.damping && (m.dampingMass > 0 || m.dampingStiffness > 0);
	if (p.useDamping) {
		damping_.Zero();
		if (m.dampingStiffness > 0) {
			ElementEnergies(x, nullptr, &damping_, false, 0.0);
			BlockMatrix scaled = pattern_;
			scaled.Zero();
			scaled.AddScaled(damping_, m.dampingStiffness);
			damping_ = scaled;
		}
		for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
			double* blk = damping_.Block(damping_.DiagonalSlot(q));
			const double w = m.dampingMass*sheet_.mass[q];
			blk[0] += w; blk[4] += w; blk[8] += w;
		}
	}
	if (barrier_) {
		if (scheme == Scheme::ImplicitMidpoint) { error = "barrier contact does not support implicit midpoint"; return false; }
		p.contact = std::make_shared<ContactFrame>();
		ContactFrame& f = *p.contact;
		barrier_->Place(tNext, f, time);
		// A feasible start: x_n (pins at their targets), carried rigidly with the
		// moving colliders wherever they could reach it during the step.  The jump
		// from x_n is checked exactly against everything that does not move with
		// it (pins and carried vertices against static colliders and the cloth).
		std::vector<double> start = x;
		bool jumped = barrier_->MovingColliders() && barrier_->Carry(x, time, tNext, start);
		if (jumped) ++stats.carried;
		for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q)
			if (fixed_[q]) { jumped = jumped || At(target, q) != At(x, q); Put(start, q, At(target, q)); }
		std::string crossing;
		if (jumped && !barrier_->IntersectionFree(f, x, start, crossing, true)) {
			error = "cloth contact: a pin or a collider-carried region passes through something (" + crossing + ")";
			return false;
		}
		barrier_->Candidates(f, start, nullptr);
		if (!(barrier_->SmallestGap(f, start) > 0)) { error = "cloth contact: the step starts inside a collider's offset"; return false; }
		barrier_->LagFriction(f, start, p.dampingScale, p.xTilde);
		// The predictor x_n + h v_n, as far as the collision detection allows.
		std::vector<double> d(dofs);
		for (std::size_t k = 0; k < dofs; ++k) d[k] = guess[k] - start[k];
		barrier_->Candidates(f, start, &d);
		const double alpha = barrier_->MaxStep(f, start, d);
		for (std::size_t k = 0; k < dofs; ++k) guess[k] = start[k] + alpha*d[k];
	}
	if (!Newton(p, guess, options, false, stats, error)) return false;
	std::vector<double> xNew(dofs), vNew(dofs);
	if (scheme == Scheme::ImplicitMidpoint) {
		for (std::size_t k = 0; k < dofs; ++k) {
			xNew[k] = 2*guess[k] - x[k];
			vNew[k] = 2*(xNew[k] - x[k])/h - v[k];
		}
	} else {
		xNew = guess;
		for (std::size_t k = 0; k < dofs; ++k) vNew[k] = a*(xNew[k] - p.xTilde[k]);
	}
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
		if (!fixed_[q]) continue;
		Put(xNew, q, At(target, q));
		Put(vNew, q, At(targetV, q));
	}
	xPrev.swap(x); vPrev.swap(v);
	x.swap(xNew); v.swap(vNew);
	hPrev = h; havePrev = true; time = tNext;
	return true;
}

bool ClothSystem::StepRecursive(double h, Scheme scheme, const NewtonOptions& options, int depth, StepStats& stats,
	std::string& error)
{
	const auto sx = x, sv = v, spx = xPrev, spv = vPrev;
	const double sh = hPrev, st = time;
	const bool sp = havePrev;
	StepStats local;
	if (StepOnce(h, scheme, options, local, error)) { stats.Add(local); return true; }
	stats.Add(local);
	x = sx; v = sv; xPrev = spx; vPrev = spv; hPrev = sh; time = st; havePrev = sp;
	if (depth >= options.maxHalvings) return false;
	++stats.halvings;
	const double t0 = time;
	if (!StepRecursive(0.5*h, scheme, options, depth + 1, stats, error)) return false;
	// The second half ends exactly at t0 + h.
	return StepRecursive((t0 + h) - time, scheme, options, depth + 1, stats, error);
}

bool ClothSystem::Step(double h, Scheme scheme, const NewtonOptions& options, StepStats& stats, std::string& error)
{
	if (!(h > 0) || !std::isfinite(h)) { error = "cloth step must be positive"; return false; }
	const double t0 = time;
	if (!StepRecursive(h, scheme, options, 0, stats, error)) return false;
	time = t0 + h;
	return true;
}

bool ClothSystem::SolveStatic(const NewtonOptions& options, StepStats& stats, std::string& error, double tau0, int stages,
	bool finish)
{
	const std::size_t dofs = x.size();
	std::vector<double> vel(dofs, 0.0);
	PinTargets(time, x, nullptr);
	Potential p;
	ExternalForce(x, vel, p.force);
	if (barrier_) {
		p.contact = std::make_shared<ContactFrame>();
		barrier_->Place(time, *p.contact);
		barrier_->Candidates(*p.contact, x, nullptr);
		if (!(barrier_->SmallestGap(*p.contact, x) > 0)) { error = "cloth contact: the sheet starts inside a collider's offset"; return false; }
	}
	NewtonOptions stage = options;
	stage.requireConvergence = false;
	stage.maxIterations = std::min(options.maxIterations, 25);
	double tau = tau0;
	for (int s = 0; s < stages; ++s, tau *= 4) {
		p.inertia = 1/(tau*tau);
		p.xHat = x;
		p.xRef = x;
		if (!Newton(p, x, stage, true, stats, error)) return false;
	}
	if (finish) {
		p.inertia = 0;
		p.xRef = x;
		if (!Newton(p, x, options, true, stats, error)) return false;
	}
	v.assign(dofs, 0.0);
	return true;
}

double ClothSystem::ContactGap(const std::vector<double>& positions, double t) const
{
	if (!barrier_) return std::numeric_limits<double>::infinity();
	ContactFrame f;
	barrier_->Place(t, f);
	barrier_->Candidates(f, positions, nullptr);
	return barrier_->SmallestGap(f, positions);
}

V3 ClothSystem::ContactForce(const std::vector<double>& positions, double t) const
{
	if (!barrier_) return {{0, 0, 0}};
	ContactFrame f;
	barrier_->Place(t, f);
	barrier_->Candidates(f, positions, nullptr);
	return barrier_->TotalForce(f, positions);
}

double ClothSystem::ContactEnergy(const std::vector<double>& positions, double t) const
{
	if (!barrier_) return 0;
	ContactFrame f;
	barrier_->Place(t, f);
	barrier_->Candidates(f, positions, nullptr);
	const std::vector<char> none(sheet_.Vertices(), 0);
	return barrier_->Evaluate(f, positions, nullptr, nullptr, false, none);
}

bool ClothSystem::PathIntersectionFree(const std::vector<double>& x0, const std::vector<double>& x1, double t,
	std::string& detail) const
{
	if (!barrier_) return true;
	ContactFrame f;
	barrier_->Place(t, f);
	return barrier_->IntersectionFree(f, x0, x1, detail);
}

double ClothSystem::ElasticEnergy(const std::vector<double>& positions) const
{
	return ElementEnergies(positions, nullptr, nullptr, false, 0.0);
}

void ClothSystem::ElasticGradient(const std::vector<double>& positions, std::vector<double>& gradient) const
{
	gradient.assign(positions.size(), 0.0);
	ElementEnergies(positions, &gradient, nullptr, false, 0.0);
}

double ClothSystem::KineticEnergy(const std::vector<double>& velocities) const
{
	double s = 0;
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
		const V3 w = At(velocities, q);
		s += 0.5*sheet_.mass[q]*Dot(w, w);
	}
	return s;
}

double ClothSystem::GravityPotential(const std::vector<double>& positions) const
{
	double s = 0;
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) s -= sheet_.mass[q]*Dot(config_.gravity, At(positions, q));
	return s;
}

void ClothSystem::VertexNormals(const std::vector<double>& positions, std::vector<double>& normals) const
{
	normals.assign(positions.size(), 0.0);
	for (const auto& t : sheet_.triangles) {
		const V3 x0 = At(positions, t.v[0]);
		const V3 nrm = Cross(Sub(At(positions, t.v[1]), x0), Sub(At(positions, t.v[2]), x0));
		for (int a = 0; a < 3; ++a) Accumulate(normals, t.v[a], nrm);
	}
	const V3 fallback = Cross(sheet_.spec.axisU, sheet_.spec.axisV);
	for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) {
		const V3 nrm = At(normals, q);
		const double l = std::sqrt(Dot(nrm, nrm));
		Put(normals, q, l > 0 ? Mul(nrm, 1/l) : fallback);
	}
}

void ClothSystem::ElasticHessian(const std::vector<double>& positions, BlockMatrix& h) const
{
	h = pattern_;
	h.Zero();
	ElementEnergies(positions, nullptr, &h, false, 0.0);
	MaskFixed(h);
}

} } }
