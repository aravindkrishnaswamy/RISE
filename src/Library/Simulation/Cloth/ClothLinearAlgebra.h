//////////////////////////////////////////////////////////////////////
//
//  ClothLinearAlgebra.h - 3x3-block sparse symmetric matrices and their
//  solvers for the cloth solver (simulation M3a)
//
//  BlockMatrix stores both triangles of a symmetric matrix in block CSR
//  (3x3 blocks, row-major inside a block, columns sorted per row), with a
//  fixed pattern built once from the element connectivity.  Two solvers:
//  preconditioned conjugate gradient with a block symmetric Gauss-Seidel
//  (default) or block-Jacobi preconditioner (the default for dynamics, where
//  the mass term keeps the system reasonably conditioned), and a banded Cholesky factorisation under the better of
//  two structured orderings (for small or narrow problems and static
//  solves, which are too ill-conditioned for CG).  Everything runs in a
//  fixed order on one thread, so results are bitwise reproducible.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_LINEAR_ALGEBRA_H
#define RISE_SIMULATION_CLOTH_LINEAR_ALGEBRA_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

class BlockMatrix {
public:
	//! Pattern: the diagonal plus both (i,j) and (j,i) for every listed pair.
	void Build(std::uint32_t blocks, const std::vector<std::pair<std::uint32_t,std::uint32_t>>& pairs);
	std::uint32_t Blocks() const { return n_; }
	//! Slot of block (i,j); the pair must be in the pattern.
	std::size_t Slot(std::uint32_t i, std::uint32_t j) const;
	std::size_t DiagonalSlot(std::uint32_t i) const { return diagonal_[i]; }
	void Zero();
	double* Block(std::size_t slot) { return &values_[9*slot]; }
	const double* Block(std::size_t slot) const { return &values_[9*slot]; }
	void Add(std::size_t slot, const double* block) { double* b = Block(slot); for (int k = 0; k < 9; ++k) b[k] += block[k]; }
	//! y = A x (3n vectors).
	void Multiply(const std::vector<double>& x, std::vector<double>& y) const;
	//! Copies values (same pattern) scaled: this += s * other.
	void AddScaled(const BlockMatrix& other, double s);
	const std::vector<std::size_t>& RowStart() const { return rowStart_; }
	const std::vector<std::uint32_t>& Column() const { return column_; }
private:
	std::uint32_t n_ = 0;
	std::vector<std::size_t> rowStart_, diagonal_;
	std::vector<std::uint32_t> column_;
	std::vector<double> values_;
};

struct LinearSolveStats {
	int iterations = 0;     // CG iterations (0 for the direct solver)
	double residual = 0;    // relative residual reached
	bool direct = false;    // the banded factorisation was used
	bool converged = false;
};

enum class Preconditioner { BlockJacobi, SymmetricGaussSeidel };
//! Solves A x = b by preconditioned CG from x = 0 (block Jacobi, or block
//! symmetric Gauss-Seidel in vertex order).  Stops at ||r|| <= rtol ||b|| or
//! maxIterations.  Returns false only on a breakdown (non-positive
//! curvature), leaving the best iterate in x.
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats,
	Preconditioner preconditioner = Preconditioner::SymmetricGaussSeidel);

//! Banded Cholesky of A under a vertex ordering.  Factor returns false on a
//! non-positive pivot (A not positive definite in floating point).
class BandedCholesky {
public:
	bool Factor(const BlockMatrix& a, const std::vector<std::uint32_t>& order);
	void Solve(const std::vector<double>& b, std::vector<double>& x) const;
	//! Scalar half-bandwidth of A under `order` (vertex order -> position).
	static std::size_t Bandwidth(const BlockMatrix& a, const std::vector<std::uint32_t>& order);
private:
	std::size_t n_ = 0, bw_ = 0;
	std::vector<std::uint32_t> position_; // vertex -> position
	std::vector<double> band_;            // row i: entries (i, i-d) for d = 0..bw at i*(bw+1)+d
};

} } }

#endif
