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
#include <cstdio>
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

namespace {
// The track's map from time t0 to time t1: x -> R1 R0^T (x - c0) + c1.
V3 TrackMap(const TransformTrack& track, const V3& x, double t0, double t1)
{
	double r0[9], c0[3], r1[9], c1[3];
	track.At(t0, r0, c0);
	track.At(t1, r1, c1);
	const double d[3] = {x[0] - c0[0], x[1] - c0[1], x[2] - c0[2]};
	double local[3];
	for (int i = 0; i < 3; ++i) local[i] = r0[i]*d[0] + r0[3 + i]*d[1] + r0[6 + i]*d[2]; // R0^T d
	V3 out;
	for (int i = 0; i < 3; ++i) out[i] = r1[3*i]*local[0] + r1[3*i + 1]*local[1] + r1[3*i + 2]*local[2] + c1[i];
	return out;
}
constexpr double TrackDifference = 1e-6; // s, half the central-difference span
}

V3 Motion::Position(const V3& rest, double t) const
{
	switch (type) {
	case Type::Track: return track ? TrackMap(*track, rest, 0.0, t) : rest;
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
	case Type::Track: {
		if (!track) return {{0, 0, 0}};
		const V3 a = TrackMap(*track, rest, 0.0, t - TrackDifference), b = TrackMap(*track, rest, 0.0, t + TrackDifference);
		return Mul(Sub(b, a), 1/(2*TrackDifference));
	}
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
	case Type::Track: return track ? TrackMap(*track, x, t0, t1) : x;
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

SampledTrack::SampledTrack(const TransformTrack& source, double t0, double t1, int pieces, const TransformTrack* start)
{
	pieces = std::max(1, pieces);
	double r[9], c[3];
	source.At(0.0, r, c);
	for (int k = 0; k < 9; ++k) zero_[k] = r[k];
	for (int k = 0; k < 3; ++k) zero_[9 + k] = c[k];
	for (int i = 0; i <= pieces; ++i) {
		const double t = i == pieces ? t1 : t0 + (t1 - t0)*i/pieces;
		(i == 0 && start ? *start : source).At(t, r, c);
		std::array<double,12> pose;
		for (int k = 0; k < 9; ++k) pose[k] = r[k];
		for (int k = 0; k < 3; ++k) pose[9 + k] = c[k];
		times_.push_back(t);
		poses_.push_back(pose);
	}
	for (int i = 0; i < pieces; ++i) {
		// Q = R_{i+1} R_i^T as axis and angle.
		const double* a = poses_[i].data();
		const double* b = poses_[i + 1].data();
		double q[9];
		for (int m = 0; m < 3; ++m) for (int n = 0; n < 3; ++n)
			q[3*m + n] = b[3*m]*a[3*n] + b[3*m + 1]*a[3*n + 1] + b[3*m + 2]*a[3*n + 2];
		const double cosine = std::max(-1.0, std::min(1.0, 0.5*(q[0] + q[4] + q[8] - 1)));
		V3 axis{{q[7] - q[5], q[2] - q[6], q[3] - q[1]}};
		const double l = std::sqrt(Dot(axis, axis));
		double angle = std::atan2(0.5*l, cosine);
		if (l > 1e-300) axis = Mul(axis, 1/l);
		else { axis = {{0, 0, 1}}; angle = 0; }
		steps_.push_back({{axis[0], axis[1], axis[2], angle}});
	}
}

void SampledTrack::At(double t, double rotation[9], double translation[3]) const
{
	if (t == 0 && (times_.empty() || times_.front() != 0)) {
		for (int k = 0; k < 9; ++k) rotation[k] = zero_[k];
		for (int k = 0; k < 3; ++k) translation[k] = zero_[9 + k];
		return;
	}
	std::size_t i = 0;
	while (i + 2 < times_.size() && t > times_[i + 1]) ++i;
	const double span = times_[i + 1] - times_[i];
	const double lambda = span > 0 ? (t - times_[i])/span : 0.0;
	const double* a = poses_[i].data();
	const double* b = poses_[i + 1].data();
	const auto& st = steps_[i];
	const double angle = lambda*st[3], cs = std::cos(angle), sn = std::sin(angle);
	const double ux = st[0], uy = st[1], uz = st[2];
	// Rodrigues: E = cos I + sin [u]x + (1 - cos) u u^T; R = E R_i.
	const double e[9] = {cs + (1 - cs)*ux*ux, (1 - cs)*ux*uy - sn*uz, (1 - cs)*ux*uz + sn*uy,
		(1 - cs)*uy*ux + sn*uz, cs + (1 - cs)*uy*uy, (1 - cs)*uy*uz - sn*ux,
		(1 - cs)*uz*ux - sn*uy, (1 - cs)*uz*uy + sn*ux, cs + (1 - cs)*uz*uz};
	for (int m = 0; m < 3; ++m) for (int n = 0; n < 3; ++n)
		rotation[3*m + n] = e[3*m]*a[n] + e[3*m + 1]*a[3 + n] + e[3*m + 2]*a[6 + n];
	for (int k = 0; k < 3; ++k) translation[k] = (1 - lambda)*a[9 + k] + lambda*b[9 + k];
}

double Motion::PathBound(const V3& x, double t0, double t1) const
{
	const double span = std::fabs(t1 - t0);
	switch (type) {
	case Type::Fixed: return 0;
	case Type::Translate: return std::sqrt(Dot(vector, vector))*span;
	case Type::Oscillate: return std::sqrt(Dot(vector, vector))*2*Pi*frequency*span;
	case Type::Rotate: {
		const V3 p = Sub(x, center);
		const V3 perp = Sub(p, Mul(axis, Dot(axis, p)));
		return std::fabs(rate)*std::sqrt(Dot(perp, perp))*span;
	}
	case Type::Track: {
		if (!track) return 0;
		double longest = 0;
		V3 prev = x;
		for (int k = 1; k <= 16; ++k) {
			const V3 q = TrackMap(*track, x, t0, t0 + (t1 - t0)*k/16.0);
			longest = std::max(longest, std::sqrt(Dot(Sub(q, prev), Sub(q, prev))));
			prev = q;
		}
		return 1.5*16*longest;
	}
	}
	return 0;
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
	if (contact) PenaltyContact(pos, gradient, hessian, energy);
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
	double energy = 0;
	if (linear_ && corotated_) {
		// M7h: the corotational model about x0 at the step's rotations, exact penalty contact.
		std::vector<double> ge;
		energy += elements_.Gradient(linearX0_, pos, ge);
		if (gradient) for (std::size_t k = 0; k < dofs; ++k) (*gradient)[k] += ge[k];
		if (hessian) hessian->AddScaled(rotatedK_, 1.0);
		PenaltyContact(pos, gradient, hessian, energy);
	} else if (linear_) {
		// M7e: the quadratic model about x0 (BeginLinear), exact penalty contact.
		std::vector<double> d(dofs), kd;
		for (std::size_t k = 0; k < dofs; ++k) d[k] = pos[k] - linearX0_[k];
		linearK0_.Multiply(d, kd, pool_.get());
		for (std::size_t k = 0; k < dofs; ++k) {
			energy += d[k]*(linearG0_[k] + 0.5*kd[k]);
			if (gradient) (*gradient)[k] += linearG0_[k] + kd[k];
		}
		if (hessian) hessian->AddScaled(linearK0_, 1.0);
		PenaltyContact(pos, gradient, hessian, energy);
	} else energy = ElementEnergies(pos, gradient, hessian, true, allowance);
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
		if (factorValid_ && a.ExtensionBlocks() == 0) {
			factor_.Solve(b, solution);
			++stats.directSolves;
			return true;
		}
		if (factorValid_) {
			// Cloth-cloth couplings outside the band: CG on the whole matrix,
			// preconditioned by the factor of its pattern part.
			LinearSolveStats ls;
			const bool ok = SolveConjugateGradient(a, b, solution, options.cgTolerance, options.cgIterations, ls, factor_, pool_.get());
			++stats.directSolves;
			stats.cgIterations += ls.iterations;
			if (!ok && indefinite) *indefinite = true;
			return ok || ls.iterations > 0;
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
		bool indefinite = false, projectedNow = !exactFirst, gradientNow = false;
		Solve(h, rhs, dx, options, preferDirect, stats, &indefinite, !hFresh);
		hFresh = false;
		double slope = 0;
		for (std::size_t k = 0; k < dofs; ++k) slope += g[k]*dx[k];
		if (exactFirst && (indefinite || !(slope < 0) || !std::isfinite(slope))) {
			++stats.projections;
			projectedNow = true;
			Evaluate(p, pos, nullptr, &h, true);
			Solve(h, rhs, dx, options, preferDirect, stats, nullptr, false);
			slope = 0;
			for (std::size_t k = 0; k < dofs; ++k) slope += g[k]*dx[k];
		}
		if (!(slope < 0) || !std::isfinite(slope)) {
			// Fall back to a diagonally scaled gradient step.
			gradientNow = true;
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
		const double ccdAlpha = alpha;
		double acceptedAlpha = 0;
		int acceptedLs = -1;
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
				acceptedAlpha = alpha; acceptedLs = ls;
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
		if (options.trace) {
			NewtonTrace t;
			t.time = time; t.h = p.traceH; t.iteration = it; t.residual = res; t.phi = phi; t.slope = slope;
			t.ccdAlpha = ccdAlpha; t.alpha = accepted ? acceptedAlpha : 0; t.lineSearch = accepted ? acceptedLs : -1;
			t.projected = projectedNow; t.gradientStep = gradientNow;
			double worstR = -1;
			for (std::uint32_t q = 0; q < n; ++q) {
				if (fixed_[q]) continue;
				const std::size_t k = 3*static_cast<std::size_t>(q);
				const double r = std::sqrt(g[k]*g[k] + g[k+1]*g[k+1] + g[k+2]*g[k+2])/sheet_.mass[q];
				if (r > worstR) { worstR = r; t.worst = q; }
			}
			if (p.contact) {
				t.candidates = p.contact->candidates.size();
				t.active = barrier_->ActivePairs(*p.contact, pos);
				t.gap = barrier_->SmallestGap(*p.contact, pos);
			}
			options.trace(t);
		}
		if (!accepted) { error = "cloth Newton line search failed"; return false; }
	}
}

void ClothSystem::PenaltyContact(const std::vector<double>& pos, std::vector<double>* gradient, BlockMatrix* hessian, double& energy) const
{
	if (config_.contact == ContactModel::Penalty && !config_.colliders.empty()) {
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
}

namespace {
double RoundF32(double x) { return static_cast<double>(static_cast<float>(x)); }
} // namespace

void ClothSystem::BeginLinearAt(const std::vector<double>& x0)
{
	linearX0_ = x0;
	linearG0_.assign(x0.size(), 0.0);
	ElementEnergies(x0, &linearG0_, nullptr, false, 0.0);
	linearK0_ = pattern_;
	linearK0_.Zero();
	ElementEnergies(x0, nullptr, &linearK0_, false, 0.0);
	for (double& g : linearG0_) g = RoundF32(g);
	for (std::size_t k = 0; k < linearK0_.PatternBlocks(); ++k) {
		double* b = linearK0_.Block(k);
		for (int r = 0; r < 9; ++r) b[r] = RoundF32(b[r]);
	}
	// The partner reads the upper blocks and mirrors them: the lower blocks are their transposes.
	for (std::uint32_t i = 0; i < linearK0_.Blocks(); ++i)
		for (std::size_t k = linearK0_.RowStart()[i]; k < linearK0_.RowStart()[i + 1]; ++k) {
			const std::uint32_t j = linearK0_.Column()[k];
			if (j >= i) continue;
			const double* u = linearK0_.Block(linearK0_.Slot(j, i));
			double* b = linearK0_.Block(k);
			for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) b[3*r + c] = u[3*c + r];
		}
	// M7h: the corotational model's elements (rotations the identity until the first step).
	LinearElements(x0, elements_);
	elements_.Balance();   // m7j
	rotatedK_ = pattern_;
	elements_.Assemble(rotatedK_);
	linear_ = true;
}

void ClothSystem::LinearElements(const std::vector<double>& pos, CorotatedElements& out) const
{
	const std::size_t nt = sheet_.triangles.size(), nh = sheet_.hinges.size(), nr = sheet_.rods.size();
	out.Resize(nt + nh + nr);
	auto put = [&](std::size_t e, CorotatedElements::Kind kind, int count, const std::uint32_t* v, const double* g, const double* h) {
		CorotatedElements::Element& el = out.elements[e];
		el.kind = kind;
		el.count = static_cast<std::uint8_t>(count);
		for (int a = 0; a < count; ++a) el.v[a] = v[a];
		const int n = 3*count;
		for (int i = 0; i < n; ++i) {
			out.gradient[12*e + static_cast<std::size_t>(i)] = RoundF32(g[i]);
			for (int j = 0; j < n; ++j) out.stiffness[144*e + static_cast<std::size_t>(12*i + j)] = RoundF32(h[n*i + j]);
		}
	};
	double g[12], h[144];
	for (std::size_t t = 0; t < nt; ++t) {
		const Triangle& tri = sheet_.triangles[t];
		const V3 xs[3] = {At(pos, tri.v[0]), At(pos, tri.v[1]), At(pos, tri.v[2])};
		MembraneElement(tri, xs, c11_, c12_, c22_, c66_, g, h, 0.0);
		put(t, CorotatedElements::Membrane, 3, tri.v, g, h);
	}
	for (std::size_t e = 0; e < nh; ++e) {
		const Hinge& hg = sheet_.hinges[e];
		const V3 xs[4] = {At(pos, hg.v[0]), At(pos, hg.v[1]), At(pos, hg.v[2]), At(pos, hg.v[3])};
		HingeElement(xs, hg.stiffness*hingeScale_[e], g, h);
		put(nt + e, CorotatedElements::Hinge, 4, hg.v, g, h);
	}
	for (std::size_t e = 0; e < nr; ++e) {
		const Rod& rd = sheet_.rods[e];
		const V3 xs[3] = {At(pos, rd.v[0]), At(pos, rd.v[1]), At(pos, rd.v[2])};
		RodElement(xs, rd.stiffness*rodScale_[e], g, h);
		put(nt + nh + e, CorotatedElements::Rod, 3, rd.v, g, h);
	}
	out.Mirror();
	std::vector<std::uint32_t> tris;
	tris.reserve(3*nt);
	for (const Triangle& t : sheet_.triangles) tris.insert(tris.end(), {t.v[0], t.v[1], t.v[2]});
	out.BuildFrames(tris);   // a structured sheet's rods always lie on a triangle's edge
}

double ClothSystem::RotationLag() const
{
	if (!linear_ || !corotated_) return 0;
	return elements_.Deviation(linearX0_, x, pool_.get());
}

void ClothSystem::BeginLinear()
{
	BeginLinearAt(x);
	// m7i: the macro step continues from the binary32 state the partner received, about which its
	// copy of the linear model is taken (the elements above are the ones offered, from the state
	// before rounding; after the first macro step the offered state is already rounded, RoundState).
	RoundState();
	for (double& c : linearX0_) c = RoundF32(c);
}

void ClothSystem::RoundState()
{
	for (double& c : x) c = RoundF32(c);
	for (double& c : v) c = RoundF32(c);
}

void ClothSystem::ExternalForce(const std::vector<double>& pos, const std::vector<double>& vel, std::vector<double>& f, bool ports) const
{
	const std::uint32_t n = sheet_.Vertices();
	f.assign(3*static_cast<std::size_t>(n), 0.0);
	for (std::uint32_t q = 0; q < n; ++q) Put(f, q, Mul(config_.gravity, sheet_.mass[q]));
	const bool pressure = config_.pressure != 0 || (ports && !portPressure.empty());
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
				const double pq = config_.pressure + (!ports || portPressure.empty() ? 0.0 : portPressure[q]);
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
	if (ports && !portTraction.empty())
		for (std::uint32_t q = 0; q < n; ++q) Accumulate(f, q, Mul(At(portTraction, q), sheet_.area[q]));
	// M7e: a per-vertex force (a liquid's hydrodynamic impulse over the macro step, held).
	if (ports && !portForce.empty())
		for (std::uint32_t q = 0; q < n; ++q) Accumulate(f, q, At(portForce, q));
}

void ClothSystem::Mechanics(std::vector<double>& force, BlockMatrix& stiffness) const
{
	ExternalForce(x, v, force, false);
	std::vector<double> g(x.size(), 0.0);
	ElementEnergies(x, &g, nullptr, true, 0.0);   // elastic and penalty contact
	for (std::size_t k = 0; k < g.size(); ++k) force[k] -= g[k];
	if (barrier_) {
		std::vector<double> c;
		ContactForces(x, time, c);
		for (std::size_t k = 0; k < c.size(); ++k) force[k] += c[k];
	}
	stiffness = pattern_;
	stiffness.Zero();
	ElementEnergies(x, nullptr, &stiffness, false, 0.0);
}

bool ClothSystem::StepOnce(double h, Scheme scheme, const NewtonOptions& options, StepStats& stats, std::string& error)
{
	const std::size_t dofs = x.size();
	const double tNext = time + h;
	Potential p;
	p.traceH = h;
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
	if (linear_ && corotated_) {
		// M7h: the elements' rotations for this step, from the predicted positions x + h v
		// (the partner's copy does the same from its own state).
		std::vector<double> predicted(dofs);
		for (std::size_t k = 0; k < dofs; ++k) predicted[k] = x[k] + h*v[k];
		elements_.Rotate(linearX0_, predicted, pool_.get());
		elements_.Assemble(rotatedK_);
	}
	const Material& m = config_.material;
	p.useDamping = config_.damping && (m.dampingMass > 0 || m.dampingStiffness > 0);
	if (p.useDamping) {
		damping_.Zero();
		if (m.dampingStiffness > 0) {
			if (linear_) damping_.AddScaled(LinearStiffness(), 1.0);   // M7e: K0, as the partner's copy (m7h: at the step's rotations)
			else ElementEnergies(x, nullptr, &damping_, false, 0.0);
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
		if (Kinematic()) {
			if (!KinematicStages(p, tNext, guess, options, stats, error)) return false;
		} else {
			// Nothing prescribed moves: start from x_n, then the predictor x_n + h v_n
			// as far as the collision detection allows.
			barrier_->Place(tNext, f, time);
			std::vector<double> start = x;
			for (std::uint32_t q = 0; q < sheet_.Vertices(); ++q) if (fixed_[q]) Put(start, q, At(target, q));
			barrier_->Candidates(f, start, nullptr);
			if (!(barrier_->SmallestGap(f, start) > 0)) { error = "cloth contact: the step starts inside a collider's offset"; return false; }
			barrier_->LagFriction(f, start, p.dampingScale, p.xTilde);
			std::vector<double> d(dofs);
			for (std::size_t k = 0; k < dofs; ++k) d[k] = guess[k] - start[k];
			barrier_->Candidates(f, start, &d);
			const double alpha = barrier_->MaxStep(f, start, d);
			for (std::size_t k = 0; k < dofs; ++k) guess[k] = start[k] + alpha*d[k];
		}
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
	if (stepObserver && p.contact) stepObserver(*p.contact, xNew, h);   // M6 coupling hook (never changes results)
	xPrev.swap(x); vPrev.swap(v);
	x.swap(xNew); v.swap(vNew);
	hPrev = h; havePrev = true; time = tNext;
	return true;
}

bool ClothSystem::Kinematic() const
{
	if (!barrier_) return false;
	if (barrier_->MovingColliders()) return true;
	for (const auto& g : config_.pins) if (!g.vertices.empty() && g.motion.Moving()) return true;
	return false;
}

// Barrier contact with moving pins or mesh colliders: kinematic continuation.
// The prescribed motions advance from the step's start in stages; each stage
// moves the colliders, the pins and the cloth vertices carried by a moving
// collider along their true (rigid) paths as far as conservative advancement
// over every candidate pair allows (BarrierContact::KinematicStep, gap kept
// above 0.1 of its value), and the cloth then relaxes against the kinematics
// of that stage by a Newton solve on the step's own incremental potential.  The
// last stage reaches the step's end; the step's Newton solve follows (with the
// colliders at the end pose, as before).  Every configuration on the way is
// connected to the previous one by a path checked against everything, so the
// step is intersection-free whatever the colliders do.  Prescribed motions that
// leave no room for the cloth's contact offset stall the stages: the step then
// fails with a diagnostic naming the cause and the time (not retried by halving).
bool ClothSystem::KinematicStages(Potential& p, double tNext, std::vector<double>& guess, const NewtonOptions& options,
	StepStats& stats, std::string& error)
{
	const std::uint32_t n = sheet_.Vertices();
	const std::size_t dofs = x.size();
	ContactFrame& f = *p.contact;
	// The colliders' velocity over the step (for friction): the chord from t_n to t_{n+1}.
	barrier_->Place(tNext, f, time);
	{
		std::vector<const Motion*> mesh(barrier_->Meshes(), nullptr);
		for (std::size_t m = 0; m < mesh.size(); ++m) mesh[m] = &barrier_->MeshMotion(static_cast<std::uint32_t>(m));
		barrier_->PlaceStage(time, f, mesh);
	}
	std::vector<double> cur = x;
	barrier_->Candidates(f, cur, nullptr);
	if (!(barrier_->SmallestGap(f, cur) > 0)) { error = "cloth contact: the step starts inside a collider's offset"; return false; }
	barrier_->LagFriction(f, cur, p.dampingScale, p.xTilde);
	const double reach = barrier_->Thickness() + barrier_->Settings().activation;
	NewtonOptions relax = options;
	relax.requireConvergence = false;
	relax.maxIterations = std::min(options.maxIterations, 12);
	KinematicStage stage;
	stage.follow.assign(n, nullptr);
	const std::size_t meshes = barrier_->Meshes();
	stage.mesh.assign(meshes, nullptr);
	// Per stage, scene-transform motions are followed as their samples at four
	// sub-steps, rigidly interpolated (SampledTrack); the others as they are.
	std::vector<Motion> meshLocal(meshes), pinLocal(config_.pins.size());
	auto local = [&](const Motion& m, Motion& out, double t0, double t1) -> const Motion* {
		if (!m.Moving()) return nullptr;
		if (m.type != Motion::Type::Track || !m.track) return &m;
		const std::shared_ptr<const TransformTrack> previous = out.track; // the last stage's path, continued without a jump
		out = m;
		out.track = std::make_shared<SampledTrack>(*m.track, t0, t1, 4, previous.get());
		return &out;
	};
	std::vector<int> owner;
	double s = time;
	int stages = 0, stalled = 0;
	bool carried = false;
	while (s < tNext) {
		stage.t0 = s; stage.t1 = tNext;
		for (std::size_t m = 0; m < meshes; ++m) stage.mesh[m] = local(barrier_->MeshMotion(static_cast<std::uint32_t>(m)), meshLocal[m], s, tNext);
		std::vector<const Motion*> pinMotion(config_.pins.size(), nullptr);
		for (std::size_t g = 0; g < config_.pins.size(); ++g) pinMotion[g] = local(config_.pins[g].motion, pinLocal[g], s, tNext);
		// Pins follow their motion; free vertices near a moving collider are carried with it.
		const double travel = barrier_->ColliderTravel(s, tNext, stage.mesh);
		barrier_->Owners(f, cur, reach + 2*travel, owner);
		double clothTravel = 0;
		for (std::uint32_t q = 0; q < n; ++q) {
			stage.follow[q] = nullptr;
			if (pinGroup_[q] >= 0) stage.follow[q] = pinMotion[static_cast<std::size_t>(pinGroup_[q])];
			else if (owner[q] >= 0) {
				stage.follow[q] = stage.mesh[static_cast<std::size_t>(owner[q])];
				carried = true;
			}
			if (stage.follow[q]) clothTravel = std::max(clothTravel, stage.follow[q]->PathBound(At(cur, q), s, tNext));
		}
		barrier_->Candidates(f, cur, nullptr, travel + clothTravel);
		ContactPair blocking;
		const double tau = barrier_->KinematicStep(f, cur, stage, &blocking);
		const double sNew = tau >= 1 ? tNext : s + tau*(tNext - s);
		if (!(sNew > s)) {
			fatal_ = true;
			error = DescribeBlock(f, cur, stage, blocking, s);
			return false;
		}
		for (std::uint32_t q = 0; q < n; ++q) {
			if (!stage.follow[q]) continue;
			if (pinGroup_[q] >= 0) Put(cur, q, stage.follow[q]->Position(At(sheet_.initial, q), sNew));
			else Put(cur, q, stage.follow[q]->Advance(At(cur, q), s, sNew));
		}
		barrier_->PlaceStage(sNew, f, stage.mesh);
		++stages;
		stalled = sNew - s < 1e-3*(tNext - time) ? stalled + 1 : 0;
		s = sNew;
		if (s >= tNext) break;
		// Relax the cloth against the kinematics at s.
		StepStats local;
		if (!Newton(p, cur, relax, false, local, error)) return false;
		stats.newtonIterations += local.newtonIterations; stats.cgIterations += local.cgIterations;
		stats.directSolves += local.directSolves; stats.projections += local.projections; stats.ccdLimited += local.ccdLimited;
		if (stalled >= 3) {
			// No progress for three stages: relax fully against these kinematics; if
			// there is still no room to advance, the prescribed motion is infeasible.
			StepStats full;
			if (!Newton(p, cur, options, false, full, error)) return false;
			stats.newtonIterations += full.newtonIterations; stats.cgIterations += full.cgIterations;
			stats.directSolves += full.directSolves; stats.projections += full.projections; stats.ccdLimited += full.ccdLimited;
			stage.t0 = s;
			barrier_->Candidates(f, cur, nullptr, travel + clothTravel);
			const double again = barrier_->KinematicStep(f, cur, stage, &blocking);
			if (again*(tNext - s) < 1e-3*(tNext - time)) {
				fatal_ = true;
				error = DescribeBlock(f, cur, stage, blocking, s);
				return false;
			}
			stalled = 0;
		}
		if (stages >= 400) {
			error = "cloth contact: the kinematic stages did not reach the end of the step (400 stages)";
			return false;
		}
	}
	// The end pose: pins exactly at their targets.  Then the predictor x_n + h v_n
	// for the free vertices, as far as the collision detection allows.
	PinTargets(tNext, cur, nullptr);
	barrier_->Place(tNext, f, time);
	{
		std::vector<double> d(dofs, 0.0);
		for (std::uint32_t q = 0; q < n; ++q)
			if (!fixed_[q]) for (int r = 0; r < 3; ++r) { const std::size_t k = 3*static_cast<std::size_t>(q) + r; d[k] = guess[k] - cur[k]; }
		barrier_->Candidates(f, cur, &d);
		const double alpha = barrier_->MaxStep(f, cur, d);
		for (std::size_t k = 0; k < dofs; ++k) cur[k] += alpha*d[k];
	}
	stats.stages += stages;
	if (carried) ++stats.carried;
	guess.swap(cur);
	return true;
}

// The diagnostic for a stalled kinematic stage: which prescribed motions pinch the cloth.
std::string ClothSystem::DescribeBlock(const ContactFrame& f, const std::vector<double>& pos, const KinematicStage& stage,
	const ContactPair& blocking, double t) const
{
	(void)stage;
	const std::uint32_t nu = sheet_.spec.verticesU;
	auto vertexName = [&](std::uint32_t q) {
		return "vertex " + std::to_string(q) + " (i " + std::to_string(q % nu) + ", j " + std::to_string(q / nu) + ")";
	};
	std::vector<std::string> causes;
	std::vector<int> groups;
	auto add = [&](const std::string& c) { if (std::find(causes.begin(), causes.end(), c) == causes.end()) causes.push_back(c); };
	auto addPin = [&](std::uint32_t q) {
		const int g = pinGroup_[q];
		if (std::find(groups.begin(), groups.end(), g) != groups.end()) return;
		groups.push_back(g);
		add("pin group " + std::to_string(g) + " (at " + vertexName(q) + ")");
	};
	std::vector<std::uint32_t> cloth;
	std::vector<std::string> colliders;
	barrier_->DescribePair(f, blocking, cloth, colliders);
	for (const auto& c : colliders) add(c);
	// The blocked pair's cloth vertices: pinned, or held by contact with
	// something else, or next to a pin.
	for (std::uint32_t q : cloth) if (fixed_[q]) addPin(q);
	for (std::uint32_t q : cloth) {
		if (fixed_[q]) continue;
		std::vector<std::string> others;
		barrier_->ContactsOf(f, pos, q, others);
		for (const auto& o : others) add(o);
	}
	for (std::uint32_t q : cloth)
		for (const auto& tri : sheet_.triangles)
			for (int k = 0; k < 3; ++k) if (tri.v[k] == q)
				for (int l = 0; l < 3; ++l) if (fixed_[tri.v[l]]) addPin(tri.v[l]);
	std::string what;
	for (std::size_t k = 0; k < causes.size(); ++k) what += (k ? (k + 1 == causes.size() ? " and " : ", ") : "") + causes[k];
	char head[160];
	std::snprintf(head, sizeof(head), "cloth contact: the prescribed motion is infeasible at t = %.6f s: ", t);
	char tail[160];
	std::snprintf(tail, sizeof(tail), " (no room for the contact offset, contact_thickness %g m)", barrier_->Thickness());
	const std::string where = cloth.empty() ? std::string("the cloth") : "the cloth at " + vertexName(cloth[0]);
	return std::string(head) + where + " is pinched between " + (causes.size() > 1 ? what : what + " and the cloth around it") + tail;
}

bool ClothSystem::StepRecursive(double h, Scheme scheme, const NewtonOptions& options, int depth, StepStats& stats,
	std::string& error)
{
	const auto sx = x, sv = v, spx = xPrev, spv = vPrev;
	const double sh = hPrev, st = time;
	const bool sp = havePrev;
	StepStats local;
	fatal_ = false;
	if (StepOnce(h, scheme, options, local, error)) { stats.Add(local); return true; }
	stats.Add(local);
	x = sx; v = sv; xPrev = spx; vPrev = spv; hPrev = sh; time = st; havePrev = sp;
	// Infeasible prescribed motion is not cured by smaller steps.
	if (fatal_ || depth >= options.maxHalvings) return false;
	++stats.halvings;
	{
		char when[96];
		std::snprintf(when, sizeof when, " (step at t = %.6f s, h = %.3e s, halved to depth %d)", st, h, depth + 1);
		stats.lastHalving = error + when;
	}
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
	bool finish, int stageIterations)
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
	stage.maxIterations = std::min(options.maxIterations, stageIterations);
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

bool ClothSystem::PathIntersectionFree(const std::vector<double>& x0, const std::vector<double>& x1, double t0, double t1,
	std::string& detail) const
{
	if (!barrier_) return true;
	ContactFrame f0, f1;
	barrier_->Place(t0, f0);
	barrier_->Place(t1, f1);
	return barrier_->IntersectionFree(f0, x0, x1, detail, false, &f1);
}

std::size_t ClothSystem::SelfContactPairs(const std::vector<double>& positions) const
{
	if (!barrier_ || !barrier_->Settings().selfCollision) return 0;
	ContactFrame f;
	barrier_->Place(time, f);
	barrier_->Candidates(f, positions, nullptr);
	f.candidates.erase(std::remove_if(f.candidates.begin(), f.candidates.end(), [](const ContactPair& p) {
		return p.kind != ContactPair::SelfVertexTriangle && p.kind != ContactPair::SelfEdgeEdge; }), f.candidates.end());
	return barrier_->ActivePairs(f, positions);
}

void ClothSystem::ContactForces(const std::vector<double>& positions, double t, std::vector<double>& forces) const
{
	forces.assign(positions.size(), 0.0);
	if (!barrier_) return;
	ContactFrame f;
	barrier_->Place(t, f);
	barrier_->Candidates(f, positions, nullptr);
	const std::vector<char> none(sheet_.Vertices(), 0);
	std::vector<double> g(positions.size(), 0.0);
	barrier_->Evaluate(f, positions, &g, nullptr, false, none);
	for (std::size_t k = 0; k < g.size(); ++k) forces[k] = -g[k];
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
