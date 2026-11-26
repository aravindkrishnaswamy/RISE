//////////////////////////////////////////////////////////////////////
//
//  ClothLinearAlgebra.h - 3x3-block sparse symmetric matrices and their
//  solvers for the cloth solver (simulation M3a)
//
//  BlockMatrix stores both triangles of a symmetric matrix in block CSR
//  (3x3 blocks, row-major inside a block, columns sorted per row), with a
//  fixed pattern built once from the element connectivity, plus an
//  extension: off-diagonal blocks outside the pattern that change from one
//  Newton iteration to the next (the couplings of cloth-cloth contact pairs).
//  Products include the extension; the preconditioners and the banded
//  factorisation see only the pattern part.  Two solvers:
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

#include "ClothParallel.h"
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
	//! Slot of block (i,j); the pair must be in the pattern or the extension.
	std::size_t Slot(std::uint32_t i, std::uint32_t j) const;
	//! Replaces the extension by the blocks (i,j) and (j,i) of the listed pairs
	//! that are not in the pattern (slots after the pattern's, in (row, column)
	//! order); their values start at zero.  An empty list removes it.
	void Extend(const std::vector<std::pair<std::uint32_t,std::uint32_t>>& pairs);
	std::size_t PatternBlocks() const { return column_.size(); }
	std::size_t ExtensionBlocks() const { return extRow_.size(); }
	const std::vector<std::uint32_t>& ExtensionRow() const { return extRow_; }
	const std::vector<std::uint32_t>& ExtensionColumn() const { return extColumn_; }
	std::size_t DiagonalSlot(std::uint32_t i) const { return diagonal_[i]; }
	void Zero();
	double* Block(std::size_t slot) { return &values_[9*slot]; }
	const double* Block(std::size_t slot) const { return &values_[9*slot]; }
	void Add(std::size_t slot, const double* block) { double* b = Block(slot); for (int k = 0; k < 9; ++k) b[k] += block[k]; }
	//! y = A x (3n vectors); rows in parallel on `pool`, then the extension in
	//! slot order on the caller (same result for any pool).
	void Multiply(const std::vector<double>& x, std::vector<double>& y, ParallelPool* pool = nullptr) const;
	//! y = (the pattern part of A) x.
	void MultiplyPattern(const std::vector<double>& x, std::vector<double>& y, ParallelPool* pool = nullptr) const;
	//! Pattern values (same pattern) scaled: this += s * other.
	void AddScaled(const BlockMatrix& other, double s);
	const std::vector<std::size_t>& RowStart() const { return rowStart_; }
	const std::vector<std::uint32_t>& Column() const { return column_; }
private:
	std::uint32_t n_ = 0;
	std::vector<std::size_t> rowStart_, diagonal_;
	std::vector<std::uint32_t> column_;
	std::vector<std::uint32_t> extRow_, extColumn_; // extension slots (pattern size + k), sorted by (row, column)
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

//! Geometric multigrid on a structured nu x nv vertex grid (vertex i + nu j),
//! used as the preconditioner of CG.  Levels coarsen by two in each grid
//! direction (the last vertex of a line is always kept); the prolongation is
//! bilinear in the grid indices and zero on Dirichlet vertices; coarse
//! operators are Galerkin, A_c = P^T A P, gathered through precomputed lists
//! of fine-block terms.  The smoother is line Gauss-Seidel: every grid row and
//! column is a block-banded system solved exactly (Cholesky), which handles
//! the strong anisotropy of a woven membrane (warp stiffness E/h^2 against
//! shear G/h^2, ratio ~75 for cotton) that point smoothers do not.  One
//! application is a V(1,1) cycle from zero: rows then columns forward, coarse
//! correction, columns then rows backward (the adjoint of the pre-smoother),
//! so the cycle is a symmetric operator, positive definite when A is.  Lines
//! are visited in colour order (lines that do not couple share a colour), so
//! the lines of a colour can run in parallel.  The coarsest level (at most
//! `coarsest` vertices) is factored by banded Cholesky.  Every sum runs in a
//! fixed order independent of the pool.
class GridMultigrid {
public:
	void Build(const BlockMatrix& fine, std::uint32_t nu, std::uint32_t nv, const std::vector<char>& fixed,
		std::uint32_t coarsest = 64);
	bool Built() const { return !levels_.empty(); }
	//! Numeric setup for the current values of `fine` (the pattern given to
	//! Build).  False when the coarsest operator is not positive definite.
	bool Setup(const BlockMatrix& fine);
	//! z = one V-cycle applied to r.
	void Apply(const std::vector<double>& r, std::vector<double>& z);
	std::size_t Levels() const { return levels_.size(); }
	//! Threads for the cycle and the setup (null: the caller only).  Results do not depend on it.
	void SetPool(ParallelPool* pool) { pool_ = pool; }
	ParallelPool* Pool() const { return pool_; }
private:
	// Block-banded Cholesky factors of the grid lines in one direction (0: rows
	// along u, 1: columns along v), for the line Gauss-Seidel smoother.
	struct LineSet {
		std::uint32_t lines = 0, length = 0, bw = 0;  // bw: scalar half-bandwidth
		std::uint32_t colors = 1;                     // lines l, l + colors, ... do not couple
		std::vector<std::uint32_t> entrySlot;         // per line: lower-band entries from the matrix
		std::vector<std::uint32_t> entryStart;        // per line, CSR into entrySlot/entryPos
		std::vector<std::uint32_t> entryPos;          // band position r*(bw+1) + (r-c), r >= c, per scalar entry
		std::vector<std::uint8_t> entryComponent;     // 3r'+c' inside the block
		std::vector<double> band;                     // lines * 3 length * (bw+1)
		std::vector<std::uint32_t> offStart, offSlot, offColumn; // per vertex: slots off its line
	};
	struct Level {
		std::uint32_t nu = 0, nv = 0;
		LineSet lineSet[2];
		BlockMatrix a;                         // unused on level 0 (the caller's matrix)
		// Prolongation from the next coarser level: per fine vertex, CSR of (coarse vertex, weight).
		std::vector<std::uint32_t> pStart, pIndex;
		std::vector<double> pWeight;
		// Restriction (the transpose), per coarse vertex in ascending fine order.
		std::vector<std::uint32_t> rStart, rIndex;
		std::vector<double> rWeight;
		// Galerkin operator of the next coarser level: per coarse slot, CSR of
		// (fine slot, weight) in ascending fine slot order.
		std::vector<std::uint32_t> cStart, cSlot;
		std::vector<double> cWeight;
		std::vector<double> b, x, r;           // work vectors
	};
	void Cycle(std::size_t level);
	void BuildLines(Level& level, const BlockMatrix& a, int direction);
	bool FactorLines(Level& level, const BlockMatrix& a, int direction);
	void LineSweep(Level& level, const BlockMatrix& a, int direction, bool forward);
	//! The pool, on levels large enough to gain from it.
	ParallelPool* LevelPool(const Level& level) const { return level.nu*level.nv >= 256 ? pool_ : nullptr; }
	const BlockMatrix& Matrix(std::size_t level) const { return level == 0 ? *fine_ : levels_[level].a; }
	std::vector<Level> levels_;
	const BlockMatrix* fine_ = nullptr;
	ParallelPool* pool_ = nullptr;
	BandedCholesky coarse_;
	std::vector<std::uint32_t> coarseOrder_;
};

//! PCG preconditioned by a banded factorisation (of the pattern part of A, for
//! a matrix with an extension).  Same conventions as below.
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, const BandedCholesky& factor, ParallelPool* pool);

//! PCG with the multigrid V-cycle as preconditioner.  Returns false on a
//! breakdown (non-positive curvature or a non-positive preconditioned
//! residual product), leaving the best iterate in x.
//! Inner products are summed in fixed chunks, vector updates and products run
//! on the multigrid's pool: the result does not depend on the thread count.
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, GridMultigrid& multigrid);

} } }

#endif
