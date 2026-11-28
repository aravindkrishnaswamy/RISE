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
//  the mass term keeps the system reasonably conditioned), and a direct
//  Cholesky factorisation (for small or narrow problems and static solves,
//  which are too ill-conditioned for CG): since m3a.8 the cloth system and the
//  multigrid's coarsest level use EnvelopeCholesky (reverse Cuthill-McKee,
//  envelope storage); BandedCholesky (a band under the caller's ordering)
//  remains for liquid.mac's copy of the sheet's response.  Everything runs in a
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

//! m3a.8: Cholesky of the pattern part of A in envelope (variable band) storage
//! under a fill-reducing vertex ordering: reverse Cuthill-McKee from a
//! pseudo-peripheral vertex, or one of the caller's candidate orderings when that
//! needs fewer operations.  Row i of L is stored from its first nonzero column
//! f_i to the diagonal; Cholesky fills only inside that envelope, so the
//! factorisation is exact (a direct solve of the same system) and does less
//! work than a band of the widest row.  The ordering is a function of the
//! pattern alone (ties broken by vertex index), computed once per pattern and
//! decoupled set and reused while both stay the same; everything runs on the
//! caller in a fixed order, so results do not depend on threads or platform.
//! `decoupled` vertices (pins) have zero off-diagonal blocks in every matrix
//! factored (MaskFixed), so their couplings are left out of the graph; should a
//! nonzero turn up there anyway, the analysis is redone with the full pattern.
class EnvelopeCholesky {
public:
	//! The vertices whose off-diagonal blocks are zero, and orderings (vertex order
	//! -> position lists) to compare with reverse Cuthill-McKee.  Invalidates the analysis.
	void Prepare(const std::vector<char>& decoupled, const std::vector<std::vector<std::uint32_t>>& candidates);
	bool Factor(const BlockMatrix& a);
	void Solve(const std::vector<double>& b, std::vector<double>& x) const;
	//! Deterministic reverse Cuthill-McKee ordering (position -> vertex) of the
	//! pattern of `a` without the couplings of `decoupled` vertices.
	static std::vector<std::uint32_t> ReverseCuthillMcKee(const BlockMatrix& a, const std::vector<char>& decoupled);
	//! Diagnostics of the current analysis: the ordering, the scalar half-bandwidth,
	//! the stored entries below the diagonal and the multiply-adds of one factorisation.
	const std::vector<std::uint32_t>& Order() const { return order_; }
	std::size_t Bandwidth() const { return bw_; }
	std::size_t Envelope() const { return envelope_; }
	double FactorOperations() const { return operations_; }
private:
	void Analyse(const BlockMatrix& a, bool useDecoupled);
	std::vector<char> decoupled_;
	std::vector<std::vector<std::uint32_t>> candidates_;
	bool analysed_ = false, usedDecoupled_ = false;
	std::vector<std::size_t> patternRowStart_;  // the analysed pattern (to detect a different one)
	std::vector<std::uint32_t> patternColumn_;
	std::size_t n_ = 0, bw_ = 0, envelope_ = 0;
	double operations_ = 0;
	std::vector<std::uint32_t> order_;          // position -> vertex
	std::vector<std::uint32_t> position_;       // vertex -> position
	std::vector<std::size_t> first_, rowStart_; // per scalar row: first column, offset in values_
	std::vector<double> values_;                // row i: columns first_[i]..i at rowStart_[i]
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
	EnvelopeCholesky coarse_;
};

//! PCG preconditioned by a banded factorisation (of the pattern part of A, for
//! a matrix with an extension).  Same conventions as below.
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, const BandedCholesky& factor, ParallelPool* pool);
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, const EnvelopeCholesky& factor, ParallelPool* pool);

//! PCG with the multigrid V-cycle as preconditioner.  Returns false on a
//! breakdown (non-positive curvature or a non-positive preconditioned
//! residual product), leaving the best iterate in x.
//! Inner products are summed in fixed chunks, vector updates and products run
//! on the multigrid's pool: the result does not depend on the thread count.
bool SolveConjugateGradient(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& x,
	double rtol, int maxIterations, LinearSolveStats& stats, GridMultigrid& multigrid);

//! M7h: a corotational linear elastic model about a configuration x0, element by
//! element (docs/SIMULATION_SOLVER_CONTRACT.md section 1).  Each element e keeps its
//! gradient g_e and projected Hessian K_e at x0 and a rotation R_e; with
//! d_e = R_e^T p_e - x0_e (per vertex) its energy is g_e . d_e + d_e^T K_e d_e / 2,
//! its gradient R_e (g_e + K_e d_e) and its Hessian R_e K_e R_e^T (symmetric,
//! positive semidefinite as K_e is).  A rigid motion of an element changes
//! neither its strain nor its stress, which the plain linear model about x0
//! (sum K_e, rotation-blind) does not do: a sheet turning by theta in a macro
//! step was stretched by theta^2 / 2.  The rotations are set by Rotate (held
//! over a step: the energy stays quadratic in the step), from a fit of each
//! element's frame vertices.  cloth.shell's linear model inside a macro step and
//! liquid.mac's copy of it use this one implementation, so both take the same
//! steps.  Every sum runs in element order on the caller: the result does not
//! depend on a thread count.
struct CorotatedElements {
	enum Kind : std::uint8_t { Membrane = 0, Hinge = 1, Rod = 2 };
	struct Element {
		std::uint8_t kind = Membrane;
		std::uint8_t count = 0;          // vertices: 3 (membrane, rod) or 4 (hinge)
		std::uint32_t v[4] = {0, 0, 0, 0};
		std::uint8_t frames = 0;         // frame vertices for the rotation's fit (3 or 4)
		std::uint32_t f[4] = {0, 0, 0, 0};
	};
	std::vector<Element> elements;
	std::vector<double> stiffness;       // 144 per element: K_e, row-major over the local dofs 3a + i, 12 wide
	std::vector<double> gradient;        // 12 per element: g_e
	std::vector<double> rotation;        // 9 per element (row-major): R_e
	bool Empty() const { return elements.empty(); }
	std::size_t Size() const { return elements.size(); }
	//! Room for `count` elements (stiffness and gradient zero, rotations the identity).
	void Resize(std::size_t count);
	//! The blocks (b, a), b > a, of every K_e set to the transposes of (a, b): a
	//! matrix carried by its upper blocks is the same on both sides of a port.
	void Mirror();
	//! m7j: makes each element's g_e and K_e exactly invariant to a translation of the element (their
	//! binary32 rounding is not): g_a -= mean_b g_b, K <- P K P with P the projection that removes an
	//! element's mean displacement (the upper blocks computed, the lower mirrored).  Without it the
	//! rounded K_e times d_e (which holds the element's rigid motion about the world origin, not only
	//! its strain) left a net elastic force on a free sheet: 2.4e-5 of its weight over a macro step
	//! (LiquidClothCouplingTest F).  The partner's copy calls it on the same values (both models then
	//! take the same steps); the ports carry the values before it.
	void Balance();
	//! The frame vertices: a membrane's own; a hinge's edge and wings in cyclic
	//! order (x0, x2, x1, x3); a rod's three and the third vertex of the first of
	//! `triangles` (3 per triangle) that holds its first edge.  False when a rod
	//! has no such triangle.
	bool BuildFrames(const std::vector<std::uint32_t>& triangles);
	//! R_e from x0 to p: the polar factor of sum_k (p_k - pbar)(x0_k - x0bar)^T +
	//! s n n0^T over the frame vertices (n, n0 the frames' unit normals, s their
	//! mean squared radius at x0), the identity for a degenerate or inverted frame.
	void Rotate(const std::vector<double>& x0, const std::vector<double>& p, ParallelPool* pool = nullptr);
	//! The largest angle (rad) between the held rotations and those from x0 to p.
	double Deviation(const std::vector<double>& x0, const std::vector<double>& p, ParallelPool* pool = nullptr) const;
	//! k = sum R_e K_e R_e^T (zeroed first; k's pattern holds every element's vertex pairs).
	void Assemble(BlockMatrix& k) const;
	//! g (3 per vertex, zeroed first) = sum R_e (g_e + K_e d_e); returns the energy.
	double Gradient(const std::vector<double>& x0, const std::vector<double>& p, std::vector<double>& g) const;
	//! The model's energy at p (Gradient without the gradient).
	double Energy(const std::vector<double>& x0, const std::vector<double>& p) const;
};

} } }

#endif
