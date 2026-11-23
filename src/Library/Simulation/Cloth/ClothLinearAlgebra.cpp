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

void BlockMatrix::Multiply(const std::vector<double>& x, std::vector<double>& y, ParallelPool* pool) const
{
	y.resize(3*static_cast<std::size_t>(n_));
	constexpr std::size_t rows = 256;
	ParallelFor(pool, Chunks(n_, rows), [&](std::size_t c) {
		const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::size_t>(n_, (c + 1)*rows));
		for (std::uint32_t i = static_cast<std::uint32_t>(c*rows); i < end; ++i) {
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
	});
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

// Inner product summed in chunks of 4096 entries (each in order), the chunk
// sums then added in order: the same for any pool.
constexpr std::size_t DotChunk = 4096;
double ChunkedDot(const std::vector<double>& a, const std::vector<double>& b, ParallelPool* pool, std::vector<double>& partial)
{
	const std::size_t chunks = Chunks(a.size(), DotChunk);
	partial.assign(chunks, 0.0);
	ParallelFor(pool, chunks, [&](std::size_t c) {
		const std::size_t end = std::min(a.size(), (c + 1)*DotChunk);
		double s = 0;
		for (std::size_t i = c*DotChunk; i < end; ++i) s += a[i]*b[i];
		partial[c] = s;
	});
	double s = 0;
	for (double p : partial) s += p;
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

namespace {
// Preconditioned CG from x = 0; `precondition(r, z)` applies the preconditioner.
template <class Precondition>
bool ConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, Precondition&& precondition)
{
	const std::size_t dofs = 3*static_cast<std::size_t>(a.Blocks());
	stats = LinearSolveStats{};
	x.assign(dofs, 0.0);
	const double bnorm = std::sqrt(Dot(b, b));
	if (bnorm == 0) { stats.converged = true; return true; }
	std::vector<double> r = b, z, p, ap;
	precondition(r, z);
	p = z;
	double rz = Dot(r, z);
	if (!(rz > 0) || !std::isfinite(rz)) { stats.residual = 1; return false; }
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
		if (!(rzNew > 0) || !std::isfinite(rzNew)) return false;
		const double beta = rzNew/rz;
		rz = rzNew;
		for (std::size_t k = 0; k < dofs; ++k) p[k] = z[k] + beta*p[k];
	}
	return true;
}

// Block-diagonal inverses; a singular diagonal block falls back to its scalar diagonal.
void DiagonalInverses(const BlockMatrix& a, std::vector<double>& pre)
{
	const std::uint32_t n = a.Blocks();
	pre.assign(9*static_cast<std::size_t>(n), 0.0);
	for (std::uint32_t i = 0; i < n; ++i) {
		const double* d = a.Block(a.DiagonalSlot(i));
		double* p = &pre[9*static_cast<std::size_t>(i)];
		if (!Invert3(d, p)) {
			for (int k = 0; k < 9; ++k) p[k] = 0;
			for (int k = 0; k < 3; ++k) p[4*k] = d[4*k] > 0 ? 1.0/d[4*k] : 1.0;
		}
	}
}

inline void Apply3(const double* p, const double* v, double* out)
{
	out[0] = p[0]*v[0] + p[1]*v[1] + p[2]*v[2];
	out[1] = p[3]*v[0] + p[4]*v[1] + p[5]*v[2];
	out[2] = p[6]*v[0] + p[7]*v[1] + p[8]*v[2];
}
}

bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, Preconditioner preconditioner)
{
	const std::uint32_t n = a.Blocks();
	const std::size_t dofs = 3*static_cast<std::size_t>(n);
	std::vector<double> pre;
	DiagonalInverses(a, pre);
	const auto& rs = a.RowStart();
	const auto& col = a.Column();
	auto apply = [&](std::uint32_t i, const double* v, double* out) { Apply3(&pre[9*static_cast<std::size_t>(i)], v, out); };
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
	return ConjugateGradient(a, b, x, rtol, maxIterations, stats, precondition);
}

bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, GridMultigrid& multigrid)
{
	ParallelPool* pool = multigrid.Pool();
	const std::size_t dofs = 3*static_cast<std::size_t>(a.Blocks());
	stats = LinearSolveStats{};
	x.assign(dofs, 0.0);
	std::vector<double> partial;
	auto dot = [&](const std::vector<double>& u, const std::vector<double>& v) { return ChunkedDot(u, v, pool, partial); };
	const double bnorm = std::sqrt(dot(b, b));
	if (bnorm == 0) { stats.converged = true; return true; }
	std::vector<double> r = b, z, p, ap;
	multigrid.Apply(r, z);
	p = z;
	double rz = dot(r, z);
	if (!(rz > 0) || !std::isfinite(rz)) { stats.residual = 1; return false; }
	constexpr std::size_t span = 3*1024;
	const std::size_t chunks = Chunks(dofs, span);
	for (int it = 0; it < maxIterations; ++it) {
		a.Multiply(p, ap, pool);
		const double pap = dot(p, ap);
		if (!(pap > 0) || !std::isfinite(pap)) {
			stats.iterations = it;
			stats.residual = std::sqrt(dot(r, r))/bnorm;
			return false;
		}
		const double alpha = rz/pap;
		ParallelFor(pool, chunks, [&](std::size_t c) {
			const std::size_t end = std::min(dofs, (c + 1)*span);
			for (std::size_t k = c*span; k < end; ++k) { x[k] += alpha*p[k]; r[k] -= alpha*ap[k]; }
		});
		const double rnorm = std::sqrt(dot(r, r));
		stats.iterations = it + 1;
		stats.residual = rnorm/bnorm;
		if (stats.residual <= rtol) { stats.converged = true; return true; }
		multigrid.Apply(r, z);
		const double rzNew = dot(r, z);
		if (!(rzNew > 0) || !std::isfinite(rzNew)) return false;
		const double beta = rzNew/rz;
		rz = rzNew;
		ParallelFor(pool, chunks, [&](std::size_t c) {
			const std::size_t end = std::min(dofs, (c + 1)*span);
			for (std::size_t k = c*span; k < end; ++k) p[k] = z[k] + beta*p[k];
		});
	}
	return true;
}

// ------------------------------------------------------------------ multigrid

namespace {
// Coarse nodes of a line of n vertices: the even indices and the last one.
std::uint32_t CoarseCount(std::uint32_t n) { return n <= 2 ? n : (n % 2 ? (n + 1)/2 : n/2 + 1); }
// 1-D bilinear weights of fine index i: (coarse index, weight) pairs.
int Parents1D(std::uint32_t i, std::uint32_t n, std::uint32_t* idx, double* w)
{
	if (n <= 2) { idx[0] = i; w[0] = 1; return 1; }
	if (i % 2 == 0) { idx[0] = i/2; w[0] = 1; return 1; }
	if (i == n - 1) { idx[0] = CoarseCount(n) - 1; w[0] = 1; return 1; }
	idx[0] = (i - 1)/2; w[0] = 0.5;
	idx[1] = (i + 1)/2; w[1] = 0.5;
	return 2;
}
}

namespace {
// In-place banded Cholesky of an n x n band (row i: entries (i, i-d) at i*(bw+1)+d).
bool FactorBand(double* band, std::size_t n, std::size_t bw)
{
	const std::size_t w = bw + 1;
	for (std::size_t j = 0; j < n; ++j) {
		const std::size_t k0 = j > bw ? j - bw : 0;
		double s = band[j*w];
		for (std::size_t k = k0; k < j; ++k) { const double l = band[j*w + (j-k)]; s -= l*l; }
		if (!(s > 0) || !std::isfinite(s)) return false;
		const double ljj = std::sqrt(s);
		band[j*w] = ljj;
		const std::size_t iEnd = std::min(n - 1, j + bw);
		for (std::size_t i = j + 1; i <= iEnd; ++i) {
			const std::size_t m0 = i > bw ? i - bw : 0;
			double t = band[i*w + (i-j)];
			for (std::size_t k = std::max(k0, m0); k < j; ++k) t -= band[i*w + (i-k)]*band[j*w + (j-k)];
			band[i*w + (i-j)] = t/ljj;
		}
	}
	return true;
}

}

void GridMultigrid::BuildLines(Level& level, const BlockMatrix& a, int d)
{
	LineSet& set = level.lineSet[d];
	const std::uint32_t nu = level.nu, nv = level.nv;
	set.lines = d == 0 ? nv : nu;
	set.length = d == 0 ? nu : nv;
	auto pos = [&](std::uint32_t q) { return d == 0 ? q % nu : q / nu; };
	auto line = [&](std::uint32_t q) { return d == 0 ? q / nu : q % nu; };
	auto vertex = [&](std::uint32_t l, std::uint32_t p) { return d == 0 ? p + nu*l : l + nu*p; };
	const auto& rs = a.RowStart();
	const auto& col = a.Column();
	std::uint32_t w = 0;
	for (std::uint32_t q = 0; q < nu*nv; ++q)
		for (std::size_t k = rs[q]; k < rs[q+1]; ++k)
			if (line(col[k]) == line(q)) w = std::max(w, pos(q) > pos(col[k]) ? pos(q) - pos(col[k]) : pos(col[k]) - pos(q));
	set.bw = 3*w + 2;
	std::uint32_t reach = 0; // largest line distance of a coupling
	for (std::uint32_t q = 0; q < nu*nv; ++q)
		for (std::size_t k = rs[q]; k < rs[q+1]; ++k) {
			const std::uint32_t a0 = line(q), a1 = line(col[k]);
			reach = std::max(reach, a0 > a1 ? a0 - a1 : a1 - a0);
		}
	set.colors = std::min(set.lines, reach + 1);
	const std::size_t width = set.bw + 1;
	set.entrySlot.clear(); set.entryPos.clear(); set.entryComponent.clear();
	set.entryStart.assign(1, 0);
	for (std::uint32_t l = 0; l < set.lines; ++l) {
		for (std::uint32_t p = 0; p < set.length; ++p) {
			const std::uint32_t q = vertex(l, p);
			for (std::size_t k = rs[q]; k < rs[q+1]; ++k) {
				const std::uint32_t c = col[k];
				if (line(c) != l || pos(c) > p) continue;
				for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) {
					const std::size_t R = 3*static_cast<std::size_t>(p) + r, C = 3*static_cast<std::size_t>(pos(c)) + cc;
					if (C > R) continue;
					set.entrySlot.push_back(static_cast<std::uint32_t>(k));
					set.entryPos.push_back(static_cast<std::uint32_t>(R*width + (R - C)));
					set.entryComponent.push_back(static_cast<std::uint8_t>(3*r + cc));
				}
			}
		}
		set.entryStart.push_back(static_cast<std::uint32_t>(set.entrySlot.size()));
	}
	set.band.assign(static_cast<std::size_t>(set.lines)*3*set.length*width, 0.0);
	set.offStart.assign(1, 0);
	set.offSlot.clear(); set.offColumn.clear();
	for (std::uint32_t q = 0; q < nu*nv; ++q) {
		for (std::size_t k = rs[q]; k < rs[q+1]; ++k)
			if (line(col[k]) != line(q)) { set.offSlot.push_back(static_cast<std::uint32_t>(k)); set.offColumn.push_back(col[k]); }
		set.offStart.push_back(static_cast<std::uint32_t>(set.offSlot.size()));
	}
}

bool GridMultigrid::FactorLines(Level& level, const BlockMatrix& a, int d)
{
	LineSet& set = level.lineSet[d];
	const std::size_t n = 3*static_cast<std::size_t>(set.length), width = set.bw + 1;
	std::vector<char> ok(set.lines, 1);
	ParallelFor(LevelPool(level), set.lines, [&](std::size_t l) {
		double* band = &set.band[l*n*width];
		std::fill(band, band + n*width, 0.0);
		for (std::uint32_t e = set.entryStart[l]; e < set.entryStart[l+1]; ++e)
			band[set.entryPos[e]] = a.Block(set.entrySlot[e])[set.entryComponent[e]];
		if (!FactorBand(band, n, set.bw)) { ok[l] = 0; return; }
		for (std::size_t i = 0; i < n; ++i) band[i*width] = 1/band[i*width];
	});
	return std::all_of(ok.begin(), ok.end(), [](char c) { return c != 0; });
}

// Line Gauss-Seidel in direction d: the lines are visited by colour (l mod
// colors, ascending when forward, descending when not).  The lines of one
// colour do not couple, so they are relaxed independently (in groups of
// four whose band solves are interleaved, which hides the latency of the
// triangular solves; the groups may run in parallel).  Each line solve is
// exact, so the backward sweep is the adjoint of the forward one.
void GridMultigrid::LineSweep(Level& level, const BlockMatrix& a, int d, bool forward)
{
	LineSet& set = level.lineSet[d];
	const std::uint32_t nu = level.nu;
	const std::size_t n = 3*static_cast<std::size_t>(set.length), width = set.bw + 1, bw = set.bw;
	std::vector<double>& x = level.x;
	const std::vector<double>& b = level.b;
	std::vector<double>& y = level.r; // scratch: line l at 3 length l
	constexpr std::uint32_t group = 4;
	for (std::uint32_t step = 0; step < set.colors; ++step) {
		const std::uint32_t color = forward ? step : set.colors - 1 - step;
		const std::uint32_t count = color < set.lines ? (set.lines - color + set.colors - 1)/set.colors : 0;
		ParallelFor(LevelPool(level), Chunks(count, group), [&](std::size_t chunk) {
			std::uint32_t lines[group];
			std::uint32_t m = 0;
			for (std::uint32_t g = 0; g < group; ++g) {
				const std::size_t index = chunk*group + g;
				if (index < count) lines[m++] = color + static_cast<std::uint32_t>(index)*set.colors;
			}
			for (std::uint32_t g = 0; g < m; ++g) {
				const std::uint32_t l = lines[g];
				for (std::uint32_t p = 0; p < set.length; ++p) {
					const std::uint32_t q = d == 0 ? p + nu*l : l + nu*p;
					double t[3] = {b[3*static_cast<std::size_t>(q)], b[3*static_cast<std::size_t>(q)+1], b[3*static_cast<std::size_t>(q)+2]};
					for (std::uint32_t e = set.offStart[q]; e < set.offStart[q+1]; ++e) {
						const double* mm = a.Block(set.offSlot[e]);
						const double* v = &x[3*static_cast<std::size_t>(set.offColumn[e])];
						t[0] -= mm[0]*v[0] + mm[1]*v[1] + mm[2]*v[2];
						t[1] -= mm[3]*v[0] + mm[4]*v[1] + mm[5]*v[2];
						t[2] -= mm[6]*v[0] + mm[7]*v[1] + mm[8]*v[2];
					}
					double* yl = &y[n*l + 3*static_cast<std::size_t>(p)];
					yl[0] = t[0]; yl[1] = t[1]; yl[2] = t[2];
				}
			}
			// L L^T y = t on each line of the group (diagonal entries hold reciprocals).
			for (std::size_t i = 0; i < n; ++i) {
				const std::size_t k0 = i > bw ? i - bw : 0;
				for (std::uint32_t g = 0; g < m; ++g) {
					const double* band = &set.band[static_cast<std::size_t>(lines[g])*n*width];
					double* yl = &y[n*lines[g]];
					double sum = yl[i];
					for (std::size_t k = k0; k < i; ++k) sum -= band[i*width + (i-k)]*yl[k];
					yl[i] = sum*band[i*width];
				}
			}
			for (std::size_t ii = n; ii-- > 0;) {
				const std::size_t kEnd = std::min(n - 1, ii + bw);
				for (std::uint32_t g = 0; g < m; ++g) {
					const double* band = &set.band[static_cast<std::size_t>(lines[g])*n*width];
					double* yl = &y[n*lines[g]];
					double sum = yl[ii];
					for (std::size_t k = ii + 1; k <= kEnd; ++k) sum -= band[k*width + (k-ii)]*yl[k];
					yl[ii] = sum*band[ii*width];
				}
			}
			for (std::uint32_t g = 0; g < m; ++g) {
				const std::uint32_t l = lines[g];
				for (std::uint32_t p = 0; p < set.length; ++p) {
					const std::uint32_t q = d == 0 ? p + nu*l : l + nu*p;
					for (int r = 0; r < 3; ++r) x[3*static_cast<std::size_t>(q) + r] = y[n*l + 3*static_cast<std::size_t>(p) + r];
				}
			}
		});
	}
}

void GridMultigrid::Build(const BlockMatrix& fine, std::uint32_t nu, std::uint32_t nv, const std::vector<char>& fixed,
	std::uint32_t coarsest)
{
	fine_ = &fine;
	levels_.clear();
	levels_.emplace_back();
	levels_[0].nu = nu; levels_[0].nv = nv;
	const BlockMatrix* pattern = &fine;
	for (std::size_t l = 0;; ++l) {
		const std::uint32_t fu = levels_[l].nu, fv = levels_[l].nv;
		const std::uint32_t cu = CoarseCount(fu), cv = CoarseCount(fv);
		if (fu*fv <= coarsest || (cu == fu && cv == fv)) break;
		Level& f = levels_[l];
		// Prolongation (zero rows on the finest level's Dirichlet vertices).
		f.pStart.assign(1, 0);
		f.pIndex.clear(); f.pWeight.clear();
		for (std::uint32_t j = 0; j < fv; ++j) for (std::uint32_t i = 0; i < fu; ++i) {
			const std::uint32_t q = i + fu*j;
			if (!(l == 0 && q < fixed.size() && fixed[q])) {
				std::uint32_t ia[2], ja[2];
				double wa[2], wb[2];
				const int na = Parents1D(i, fu, ia, wa), nb = Parents1D(j, fv, ja, wb);
				for (int b = 0; b < nb; ++b) for (int a = 0; a < na; ++a) {
					f.pIndex.push_back(ia[a] + cu*ja[b]);
					f.pWeight.push_back(wa[a]*wb[b]);
				}
			}
			f.pStart.push_back(static_cast<std::uint32_t>(f.pIndex.size()));
		}
		// Restriction: the transpose, per coarse vertex in ascending fine order.
		{
			std::vector<std::uint32_t> count(static_cast<std::size_t>(cu)*cv + 1, 0);
			for (std::uint32_t e = 0; e < f.pIndex.size(); ++e) ++count[f.pIndex[e] + 1];
			for (std::size_t k = 1; k < count.size(); ++k) count[k] += count[k-1];
			f.rStart = count;
			f.rIndex.assign(f.pIndex.size(), 0); f.rWeight.assign(f.pIndex.size(), 0.0);
			for (std::uint32_t i = 0; i < fu*fv; ++i)
				for (std::uint32_t e = f.pStart[i]; e < f.pStart[i+1]; ++e) {
					const std::uint32_t at = count[f.pIndex[e]]++;
					f.rIndex[at] = i; f.rWeight[at] = f.pWeight[e];
				}
		}
		// Coarse pattern from P^T A P.
		std::vector<std::pair<std::uint32_t,std::uint32_t>> pairs;
		const auto& rs = pattern->RowStart();
		const auto& col = pattern->Column();
		for (std::uint32_t i = 0; i < fu*fv; ++i)
			for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
				const std::uint32_t j = col[k];
				if (j < i) continue;
				for (std::uint32_t a = f.pStart[i]; a < f.pStart[i+1]; ++a)
					for (std::uint32_t b = f.pStart[j]; b < f.pStart[j+1]; ++b) pairs.emplace_back(f.pIndex[a], f.pIndex[b]);
			}
		Level c;
		c.nu = cu; c.nv = cv;
		c.a.Build(cu*cv, pairs);
		// Galerkin gather lists: per coarse slot, its (fine slot, weight) terms in ascending fine slot order.
		{
			std::vector<std::uint32_t> target;
			std::vector<std::uint32_t> source;
			std::vector<double> weight;
			for (std::uint32_t i = 0; i < fu*fv; ++i)
				for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
					const std::uint32_t j = col[k];
					for (std::uint32_t a = f.pStart[i]; a < f.pStart[i+1]; ++a)
						for (std::uint32_t b = f.pStart[j]; b < f.pStart[j+1]; ++b) {
							target.push_back(static_cast<std::uint32_t>(c.a.Slot(f.pIndex[a], f.pIndex[b])));
							source.push_back(static_cast<std::uint32_t>(k));
							weight.push_back(f.pWeight[a]*f.pWeight[b]);
						}
				}
			std::vector<std::uint32_t> count(c.a.Column().size() + 1, 0);
			for (std::uint32_t t : target) ++count[t + 1];
			for (std::size_t k = 1; k < count.size(); ++k) count[k] += count[k-1];
			f.cStart = count;
			f.cSlot.assign(target.size(), 0); f.cWeight.assign(target.size(), 0.0);
			for (std::size_t e = 0; e < target.size(); ++e) {
				const std::uint32_t at = count[target[e]]++;
				f.cSlot[at] = source[e]; f.cWeight[at] = weight[e];
			}
		}
		levels_.push_back(std::move(c));
		pattern = &levels_.back().a;
	}
	for (auto& level : levels_) {
		const std::size_t dofs = 3*static_cast<std::size_t>(level.nu)*level.nv;
		level.b.assign(dofs, 0.0); level.x.assign(dofs, 0.0); level.r.assign(dofs, 0.0);
	}
	for (std::size_t l = 0; l + 1 < levels_.size(); ++l) {
		BuildLines(levels_[l], Matrix(l), 0);
		BuildLines(levels_[l], Matrix(l), 1);
	}
	coarseOrder_.resize(static_cast<std::size_t>(levels_.back().nu)*levels_.back().nv);
	for (std::uint32_t k = 0; k < coarseOrder_.size(); ++k) coarseOrder_[k] = k;
}

bool GridMultigrid::Setup(const BlockMatrix& fine)
{
	fine_ = &fine;
	for (std::size_t l = 0; l + 1 < levels_.size(); ++l) {
		const BlockMatrix& a = Matrix(l);
		BlockMatrix& c = levels_[l+1].a;
		const Level& f = levels_[l];
		const auto& rs = c.RowStart();
		constexpr std::size_t rows = 64;
		ParallelFor(LevelPool(f), Chunks(c.Blocks(), rows), [&](std::size_t chunk) {
			const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::size_t>(c.Blocks(), (chunk + 1)*rows));
			for (std::uint32_t i = static_cast<std::uint32_t>(chunk*rows); i < end; ++i) {
				for (std::size_t k = rs[i]; k < rs[i+1]; ++k) {
					double* dst = c.Block(k);
					for (int q = 0; q < 9; ++q) dst[q] = 0;
					for (std::uint32_t e = f.cStart[k]; e < f.cStart[k+1]; ++e) {
						const double* blk = a.Block(f.cSlot[e]);
						const double w = f.cWeight[e];
						for (int q = 0; q < 9; ++q) dst[q] += w*blk[q];
					}
				}
				// A coarse vertex whose fine children are all Dirichlet has no entries.
				double* d = c.Block(c.DiagonalSlot(i));
				bool zero = true;
				for (int q = 0; q < 9; ++q) zero = zero && d[q] == 0;
				if (zero) d[0] = d[4] = d[8] = 1;
			}
		});
	}
	for (std::size_t l = 0; l + 1 < levels_.size(); ++l)
		if (!FactorLines(levels_[l], Matrix(l), 0) || !FactorLines(levels_[l], Matrix(l), 1)) return false;
	return coarse_.Factor(Matrix(levels_.size() - 1), coarseOrder_);
}

void GridMultigrid::Cycle(std::size_t l)
{
	Level& f = levels_[l];
	if (l + 1 == levels_.size()) { coarse_.Solve(f.b, f.x); return; }
	const BlockMatrix& a = Matrix(l);
	const std::uint32_t n = a.Blocks();
	ParallelPool* pool = LevelPool(f);
	std::vector<double>& x = f.x;
	const std::vector<double>& b = f.b;
	std::fill(x.begin(), x.end(), 0.0);
	LineSweep(f, a, 0, true);
	LineSweep(f, a, 1, true);
	a.Multiply(x, f.r, pool);
	Level& c = levels_[l+1];
	constexpr std::size_t span = 256;
	ParallelFor(pool, Chunks(static_cast<std::size_t>(c.nu)*c.nv, span), [&](std::size_t chunk) {
		const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(c.nu)*c.nv, (chunk + 1)*span);
		for (std::size_t I = chunk*span; I < end; ++I) {
			double s0 = 0, s1 = 0, s2 = 0;
			for (std::uint32_t e = f.rStart[I]; e < f.rStart[I+1]; ++e) {
				const double w = f.rWeight[e];
				const std::size_t q = 3*static_cast<std::size_t>(f.rIndex[e]);
				s0 += w*(b[q] - f.r[q]); s1 += w*(b[q+1] - f.r[q+1]); s2 += w*(b[q+2] - f.r[q+2]);
			}
			c.b[3*I] = s0; c.b[3*I+1] = s1; c.b[3*I+2] = s2;
		}
	});
	Cycle(l + 1);
	ParallelFor(pool, Chunks(n, span), [&](std::size_t chunk) {
		const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::size_t>(n, (chunk + 1)*span));
		for (std::uint32_t i = static_cast<std::uint32_t>(chunk*span); i < end; ++i)
			for (std::uint32_t e = f.pStart[i]; e < f.pStart[i+1]; ++e) {
				const double w = f.pWeight[e];
				const std::size_t I = 3*static_cast<std::size_t>(f.pIndex[e]), q = 3*static_cast<std::size_t>(i);
				x[q] += w*c.x[I]; x[q+1] += w*c.x[I+1]; x[q+2] += w*c.x[I+2];
			}
	});
	LineSweep(f, a, 1, false);
	LineSweep(f, a, 0, false);
}

void GridMultigrid::Apply(const std::vector<double>& r, std::vector<double>& z)
{
	levels_[0].b = r;
	Cycle(0);
	z = levels_[0].x;
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
