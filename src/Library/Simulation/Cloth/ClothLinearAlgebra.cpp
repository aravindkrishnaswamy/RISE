//////////////////////////////////////////////////////////////////////
//
//  ClothLinearAlgebra.cpp - Block sparse matrix, PCG, banded Cholesky
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothLinearAlgebra.h"

#include <algorithm>
#include <cmath>

namespace RISE { namespace Simulation { namespace Cloth {

void BlockMatrix::Build(std::uint32_t blocks, const std::vector<std::pair<std::uint32_t,std::uint32_t>>& pairs)
{
	n_ = blocks;
	std::vector<std::vector<std::uint32_t>> rows(blocks);
	for (std::uint32_t i = 0; i < blocks; ++i) rows[i].push_back(i);
	for (const auto& p : pairs) {
		if (p.first == p.second) continue;
		rows[p.first].push_back(p.second);
		rows[p.second].push_back(p.first);
	}
	rowStart_.assign(blocks + 1, 0);
	column_.clear();
	diagonal_.assign(blocks, 0);
	for (std::uint32_t i = 0; i < blocks; ++i) {
		auto& r = rows[i];
		std::sort(r.begin(), r.end());
		r.erase(std::unique(r.begin(), r.end()), r.end());
		rowStart_[i] = column_.size();
		for (std::uint32_t j : r) {
			if (j == i) diagonal_[i] = column_.size();
			column_.push_back(j);
		}
	}
	rowStart_[blocks] = column_.size();
	values_.assign(9*column_.size(), 0.0);
}

std::size_t BlockMatrix::Slot(std::uint32_t i, std::uint32_t j) const
{
	const auto begin = column_.begin() + static_cast<std::ptrdiff_t>(rowStart_[i]);
	const auto end = column_.begin() + static_cast<std::ptrdiff_t>(rowStart_[i+1]);
	const auto it = std::lower_bound(begin, end, j);
	return static_cast<std::size_t>(it - column_.begin());
}

void BlockMatrix::Zero()
{
	std::fill(values_.begin(), values_.end(), 0.0);
}

void BlockMatrix::Multiply(const std::vector<double>& x, std::vector<double>& y) const
{
	y.assign(3*static_cast<std::size_t>(n_), 0.0);
	for (std::uint32_t i = 0; i < n_; ++i) {
		double s0 = 0, s1 = 0, s2 = 0;
		for (std::size_t k = rowStart_[i]; k < rowStart_[i+1]; ++k) {
			const double* b = &values_[9*k];
			const double* v = &x[3*static_cast<std::size_t>(column_[k])];
			s0 += b[0]*v[0] + b[1]*v[1] + b[2]*v[2];
			s1 += b[3]*v[0] + b[4]*v[1] + b[5]*v[2];
			s2 += b[6]*v[0] + b[7]*v[1] + b[8]*v[2];
		}
		y[3*i] = s0; y[3*i+1] = s1; y[3*i+2] = s2;
	}
}

void BlockMatrix::AddScaled(const BlockMatrix& other, double s)
{
	for (std::size_t k = 0; k < values_.size(); ++k) values_[k] += s*other.values_[k];
}

namespace {
double Dot(const std::vector<double>& a, const std::vector<double>& b)
{
	double s = 0;
	for (std::size_t i = 0; i < a.size(); ++i) s += a[i]*b[i];
	return s;
}

// Inverse of a symmetric 3x3 block (cofactors); false when not invertible.
bool Invert3(const double* m, double* inv)
{
	const double c00 = m[4]*m[8] - m[5]*m[7], c01 = m[5]*m[6] - m[3]*m[8], c02 = m[3]*m[7] - m[4]*m[6];
	const double det = m[0]*c00 + m[1]*c01 + m[2]*c02;
	if (!(std::fabs(det) > 0) || !std::isfinite(det)) return false;
	const double d = 1.0/det;
	inv[0] = c00*d; inv[1] = (m[2]*m[7] - m[1]*m[8])*d; inv[2] = (m[1]*m[5] - m[2]*m[4])*d;
	inv[3] = c01*d; inv[4] = (m[0]*m[8] - m[2]*m[6])*d; inv[5] = (m[2]*m[3] - m[0]*m[5])*d;
	inv[6] = c02*d; inv[7] = (m[1]*m[6] - m[0]*m[7])*d; inv[8] = (m[0]*m[4] - m[1]*m[3])*d;
	return true;
}
}

bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, Preconditioner preconditioner)
{
	const std::uint32_t n = a.Blocks();
	const std::size_t dofs = 3*static_cast<std::size_t>(n);
	stats = LinearSolveStats{};
	x.assign(dofs, 0.0);
	// Block-Jacobi preconditioner; a singular diagonal block falls back to its scalar diagonal.
	std::vector<double> pre(9*static_cast<std::size_t>(n), 0.0);
	for (std::uint32_t i = 0; i < n; ++i) {
		const double* d = a.Block(a.DiagonalSlot(i));
		double* p = &pre[9*static_cast<std::size_t>(i)];
		if (!Invert3(d, p)) {
			for (int k = 0; k < 9; ++k) p[k] = 0;
			for (int k = 0; k < 3; ++k) p[4*k] = d[4*k] > 0 ? 1.0/d[4*k] : 1.0;
		}
	}
	const auto& rs = a.RowStart();
	const auto& col = a.Column();
	auto apply = [&](std::uint32_t i, const double* v, double* out) {
		const double* p = &pre[9*static_cast<std::size_t>(i)];
		out[0] = p[0]*v[0] + p[1]*v[1] + p[2]*v[2];
		out[1] = p[3]*v[0] + p[4]*v[1] + p[5]*v[2];
		out[2] = p[6]*v[0] + p[7]*v[1] + p[8]*v[2];
	};
	// Block symmetric Gauss-Seidel, M = (D + L) D^-1 (D + U): a forward then a
	// backward block sweep in vertex order (symmetric positive definite when A is).
	auto precondition = [&](const std::vector<double>& r, std::vector<double>& z) {
		z.assign(dofs, 0.0);
		if (preconditioner == Preconditioner::BlockJacobi) {
			for (std::uint32_t i = 0; i < n; ++i) apply(i, &r[3*static_cast<std::size_t>(i)], &z[3*static_cast<std::size_t>(i)]);
			return;
		}
		for (std::uint32_t i = 0; i < n; ++i) {
			double t[3] = {r[3*i], r[3*i+1], r[3*i+2]};
			for (std::size_t k = rs[i]; k < rs[i+1] && col[k] < i; ++k) {
				const double* b = a.Block(k);
				const double* v = &z[3*static_cast<std::size_t>(col[k])];
				t[0] -= b[0]*v[0] + b[1]*v[1] + b[2]*v[2];
				t[1] -= b[3]*v[0] + b[4]*v[1] + b[5]*v[2];
				t[2] -= b[6]*v[0] + b[7]*v[1] + b[8]*v[2];
			}
			apply(i, t, &z[3*static_cast<std::size_t>(i)]);
		}
		for (std::uint32_t ii = n; ii-- > 0;) {
			double t[3] = {0, 0, 0};
			for (std::size_t k = rs[ii+1]; k-- > rs[ii] && col[k] > ii;) {
				const double* b = a.Block(k);
				const double* v = &z[3*static_cast<std::size_t>(col[k])];
				t[0] += b[0]*v[0] + b[1]*v[1] + b[2]*v[2];
				t[1] += b[3]*v[0] + b[4]*v[1] + b[5]*v[2];
				t[2] += b[6]*v[0] + b[7]*v[1] + b[8]*v[2];
			}
			double c[3];
			apply(ii, t, c);
			z[3*ii] -= c[0]; z[3*ii+1] -= c[1]; z[3*ii+2] -= c[2];
		}
	};
	const double bnorm = std::sqrt(Dot(b, b));
	if (bnorm == 0) { stats.converged = true; return true; }
	std::vector<double> r = b, z, p, ap;
	precondition(r, z);
	p = z;
	double rz = Dot(r, z);
	for (int it = 0; it < maxIterations; ++it) {
		a.Multiply(p, ap);
		const double pap = Dot(p, ap);
		if (!(pap > 0) || !std::isfinite(pap)) {
			stats.iterations = it;
			stats.residual = std::sqrt(Dot(r, r))/bnorm;
			return false;
		}
		const double alpha = rz/pap;
		for (std::size_t k = 0; k < dofs; ++k) { x[k] += alpha*p[k]; r[k] -= alpha*ap[k]; }
		const double rnorm = std::sqrt(Dot(r, r));
		stats.iterations = it + 1;
		stats.residual = rnorm/bnorm;
		if (stats.residual <= rtol) { stats.converged = true; return true; }
		precondition(r, z);
		const double rzNew = Dot(r, z);
		const double beta = rzNew/rz;
		rz = rzNew;
		for (std::size_t k = 0; k < dofs; ++k) p[k] = z[k] + beta*p[k];
	}
	return true;
}

std::size_t BandedCholesky::Bandwidth(const BlockMatrix& a, const std::vector<std::uint32_t>& order)
{
	std::vector<std::uint32_t> position(order.size());
	for (std::size_t k = 0; k < order.size(); ++k) position[order[k]] = static_cast<std::uint32_t>(k);
	std::size_t bw = 0;
	const auto& rs = a.RowStart();
	const auto& col = a.Column();
	for (std::uint32_t i = 0; i < a.Blocks(); ++i)
		for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
			const std::size_t pi = position[i], pj = position[col[k]];
			bw = std::max(bw, pi > pj ? pi - pj : pj - pi);
		}
	return 3*bw + 2;
}

bool BandedCholesky::Factor(const BlockMatrix& a, const std::vector<std::uint32_t>& order)
{
	const std::uint32_t nb = a.Blocks();
	n_ = 3*static_cast<std::size_t>(nb);
	position_.assign(nb, 0);
	for (std::size_t k = 0; k < order.size(); ++k) position_[order[k]] = static_cast<std::uint32_t>(k);
	bw_ = Bandwidth(a, order);
	const std::size_t w = bw_ + 1;
	band_.assign(n_*w, 0.0);
	const auto& rs = a.RowStart();
	const auto& col = a.Column();
	for (std::uint32_t i = 0; i < nb; ++i)
		for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
			const double* b = a.Block(k);
			for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) {
				const std::size_t gi = 3*static_cast<std::size_t>(position_[i]) + r;
				const std::size_t gj = 3*static_cast<std::size_t>(position_[col[k]]) + c;
				if (gj > gi) continue; // lower triangle only
				band_[gi*w + (gi - gj)] = b[3*r + c];
			}
		}
	for (std::size_t j = 0; j < n_; ++j) {
		const std::size_t k0 = j > bw_ ? j - bw_ : 0;
		double s = band_[j*w];
		for (std::size_t k = k0; k < j; ++k) { const double l = band_[j*w + (j-k)]; s -= l*l; }
		if (!(s > 0) || !std::isfinite(s)) return false;
		const double ljj = std::sqrt(s);
		band_[j*w] = ljj;
		const std::size_t iEnd = std::min(n_ - 1, j + bw_);
		for (std::size_t i = j + 1; i <= iEnd; ++i) {
			const std::size_t m0 = i > bw_ ? i - bw_ : 0;
			double t = band_[i*w + (i-j)];
			for (std::size_t k = std::max(k0, m0); k < j; ++k) t -= band_[i*w + (i-k)]*band_[j*w + (j-k)];
			band_[i*w + (i-j)] = t/ljj;
		}
	}
	return true;
}

void BandedCholesky::Solve(const std::vector<double>& b, std::vector<double>& x) const
{
	const std::size_t w = bw_ + 1;
	std::vector<double> y(n_);
	for (std::uint32_t v = 0; v < position_.size(); ++v)
		for (int c = 0; c < 3; ++c) y[3*static_cast<std::size_t>(position_[v]) + c] = b[3*static_cast<std::size_t>(v) + c];
	for (std::size_t i = 0; i < n_; ++i) {
		const std::size_t k0 = i > bw_ ? i - bw_ : 0;
		double s = y[i];
		for (std::size_t k = k0; k < i; ++k) s -= band_[i*w + (i-k)]*y[k];
		y[i] = s/band_[i*w];
	}
	for (std::size_t ii = n_; ii-- > 0;) {
		const std::size_t kEnd = std::min(n_ - 1, ii + bw_);
		double s = y[ii];
		for (std::size_t k = ii + 1; k <= kEnd; ++k) s -= band_[k*w + (k-ii)]*y[k];
		y[ii] = s/band_[ii*w];
	}
	x.assign(n_, 0.0);
	for (std::uint32_t v = 0; v < position_.size(); ++v)
		for (int c = 0; c < 3; ++c) x[3*static_cast<std::size_t>(v) + c] = y[3*static_cast<std::size_t>(position_[v]) + c];
}

} } }
