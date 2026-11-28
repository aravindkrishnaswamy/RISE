//////////////////////////////////////////////////////////////////////
//
//  LiquidLevelSet.h - The liquid's interface (simulation M7): a level set
//  phi at cell centres (negative in the liquid), advected by fifth-order
//  WENO in space (Jiang and Peng 2000, the Hamilton-Jacobi form of Osher
//  and Fedkiw 2003 section 3.4) and third-order SSP Runge-Kutta in time
//  (Shu and Osher 1988); reinitialised by the PDE
//  phi_tau + S(phi0)(|grad phi| - 1) = 0 (Sussman, Smereka and Osher 1994;
//  Godunov Hamiltonian on the WENO one-sided derivatives) with or without
//  the subcell fix of Russo and Smereka 2000; corrected by marker particles
//  (the particle level set of Enright, Fedkiw, Ferziger and Mitchell 2002)
//  when enabled; and shifted once per step so that its liquid volume stays
//  at the initial one (the correction magnitude is returned and reported).
//
//  Every operation is deterministic and independent of the thread count
//  (LiquidGrid.h).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_LEVEL_SET_H
#define RISE_SIMULATION_LIQUID_LEVEL_SET_H

#include "LiquidGrid.h"

#include <functional>

namespace RISE { namespace Simulation { namespace Liquid {

//! Ghost values beyond an array's ends: Linear extrapolation from the two
//! outermost samples, or Constant (the outermost sample).
enum class Extension { Linear, Constant };

//! The fifth-order WENO approximation of a one-sided derivative from five
//! consecutive differences (v1..v5 = D phi at i-2 .. i+2 for phi_x^-,
//! i+2 .. i-2 for phi_x^+); Osher and Fedkiw 2003, eqs. 3.28-3.35.
double Weno5(double v1, double v2, double v3, double v4, double v5);

//! m7f: grid faces that the level set's stencils do not read across (the faces
//! an implicitly coupled sheet closes; docs/SIMULATION_SOLVER_CONTRACT.md).  The two
//! sides of a sheet are separate regions of the level set; a stencil that reads
//! across one carries one side's values to the other (the WENO advection, the
//! reinitialisation, the gradient that triggers it).
//! m7l (docs/SIMULATION_SOLVER_CONTRACT.md): the level set is two-valued at a sheet.  The
//! cells within a stencil's reach of a closed face (`second`) hold, besides their own
//! value (their own side's level set, phi), the level set of the sheet's other side
//! extended across the sheet to them along the sheet's normal (as a rigid solid's cells
//! hold the liquid's; LiquidModel ExtrapolateEnclosed): a stencil that crosses a closed
//! face reads those second values beyond it, so each side's level set is advected and
//! reinitialised with its own values on both sides of the sheet, and nothing of one
//! side reaches the other.  The second values are not state: they are evaluated from the
//! current field (every Runge-Kutta stage and reinitialisation stage) by linear recipes
//! that the sheet's geometry fixes.  A stencil that cannot read a second value (a cell
//! of no single side: a fold, two sheets; the second value undefined; a second closed
//! face) sees the last value it reached instead (constant extension, m7f's wall: zero
//! normal slope, the 90 degree contact condition of a wall).  m7f-m7k held the level set
//! beside a sheet holding liquid on one side at the signed distance to the sheet (a
//! Dirichlet condition, LiquidModel PinSheetInterface, removed in m7l), which with one
//! value per cell could not say where a thin layer on a curved sheet ends: a bag's 2.4
//! cells lost 41% in 2 s and a held bowl's 1.3 cells 44% (docs/SIMULATION_SOLVER_CONTRACT.md).
//! Beyond the domain the level set's linear extension stays.  Empty: no change to
//! any arithmetic.
struct LevelSetBarrier {
	std::array<std::vector<std::uint8_t>,3> closed;   // per axis, per face: 0 open, 1 closed (empty: none)
	//! One stencil sample: f[i1] + s (f[i1] - f[i2]); m7l, `second`: the second value of slot i1.
	struct Sample { std::size_t i1 = 0, i2 = 0; double s = 0; bool outside = false; bool second = false; };   // outside (m7g): beyond the domain's edge
	//! Per axis: the cells whose seven-point stencil (offsets -3..3) reaches past a
	//! closed face (sorted), and their samples (7 per cell).
	std::array<std::vector<std::size_t>,3> cells;
	std::array<std::vector<Sample>,3> samples;
	//! m7l: the second values.  Slot k (cells[k], sorted) has the side `side[k]` (+-(sheet + 1):
	//! the cell's centre in front of (+) or behind (-) that sheet; 0: no single side) and, when
	//! `valid[k]`, the value of the other side's level set at the cell
	//!   v = sum_t w0_t f[term_t] + clamp(-reach_k sum_t w1_t f[term_t], -reach_k, reach_k)
	//! (the level set at the point x_o 1.5 h beyond the sheet's plane on the other side, the
	//! foot of the cell's centre moved along the normal, continued linearly to the cell with
	//! the normal difference to the point a cell further, |slope| <= 1; terms [start[k], start[k + 1])).
	//! A dry side (`dry`, cells [dryStart[k], dryStart[k + 1]): the other side's cells of the trilinear
	//! stencil at the foot of the cell's centre on the sheet's plane, a quarter cell to that side): when
	//! the level set at x_o is >= 0 and at all of them >= dryLevel (h / 4), v = max(v, their least f): a
	//! side with no liquid at the sheet gets none across it.  Without this, the linear extension of a
	//! dry side's level set falling towards the sheet (the distance to the liquid on the other side that
	//! a scene's initial level set gives it) put a zero at the sheet, and the dry side's cells beside it
	//! hovered about zero: a held bowl's dry side turned 1,234 cell-steps wet in 2 s, a bag's held 1.2%
	//! of its volume below the sheet.  Without the margin, a liquid side whose cells at the sheet had
	//! dried to about zero (a bag's corners at its pinned rim) stayed dry: the bag lost 7.8% against 4.0%.
	struct Second {
		std::vector<std::size_t> cells;
		std::vector<std::int32_t> side;
		std::vector<std::uint8_t> valid;
		std::vector<std::uint32_t> start, term;
		std::vector<double> w0, w1, reach;
		std::vector<std::uint32_t> dryStart, dry;
		double dryLevel = 0;
		bool Empty() const { return cells.empty(); }
		//! The slot of a cell, or -1.
		std::ptrdiff_t Slot(std::size_t cell) const;
		//! out[k] = the second value of slot k from f (0 where not valid).
		void Evaluate(const std::vector<double>& f, std::vector<double>& out, Pool* pool) const;
		//! One recipe's value from f.
		double Value(std::size_t k, const std::vector<double>& f) const;
	};
	Second second;
	bool Empty() const { return cells[0].empty() && cells[1].empty() && cells[2].empty(); }
	//! Whether some stencil reads a second value.
	bool ReadsSecond() const { return reads; }
	bool Closed(int axis, std::size_t face) const { return !closed[std::size_t(axis)].empty() && closed[std::size_t(axis)][face] == 1; }
	//! `faces`: the closed faces per axis (any order, one entry per face; the kind 1); `second`
	//! (m7l; may be empty): the second values' slots and recipes.
	void Build(const Grid& g, const std::array<std::vector<std::pair<std::size_t, std::uint8_t>>,3>& faces, Second second);
	void Build(const Grid& g, const std::array<std::vector<std::pair<std::size_t, std::uint8_t>>,3>& faces);
	void Clear();
	bool reads = false;
	//! The coordinate along `axis` reached from the cell idx by `offset` steps, stopping
	//! at a wall face or the domain's edge.
	int Reach(const Grid& g, const int idx[3], int axis, int offset) const;
};

//! One-sided WENO5 derivatives of f (dimensions d, spacing h) along `axis`,
//! row by row: emit(first index of the row, dminus[], dplus[]) for every row
//! (rows run along the first index).  Rows are processed in fixed chunks.
//! `barrier` (m7f; cell-centred f only, Linear extension): closed faces as walls.
//! `second` (m7l): the barrier's second values of f (LevelSetBarrier::Second::Evaluate;
//! null: evaluated here when the barrier's stencils read them).
void OneSidedDerivatives(const std::array<int,3>& d, double h, const std::vector<double>& f, int axis,
	const std::array<Extension,3>& ext, Pool* pool,
	const std::function<void(std::size_t row, const double* dm, const double* dp)>& emit,
	const LevelSetBarrier* barrier = nullptr, const std::vector<double>* second = nullptr);

//! rhs = -(c . grad f) with upwinded WENO5 derivatives (`assign`: rhs is
//! overwritten, else accumulated).  c: three arrays of f's dimensions.
void UpwindAdvectionRhs(const std::array<int,3>& d, double h, const std::vector<double>& f,
	const std::array<const std::vector<double>*,3>& c, const std::array<Extension,3>& ext,
	std::vector<double>& rhs, Pool* pool, const LevelSetBarrier* barrier = nullptr);

//! The cell-centred advecting velocity at a (stage) time.
using CellVelocityAt = std::function<void(double time, std::array<std::vector<double>,3>& velocity)>;

//! One SSP-RK3 step of phi_t + u . grad phi = 0 over [t, t + dt]; the
//! velocity is asked for at the stage times t, t + dt, t + dt/2 (a solver
//! passes the same field each time).
void AdvectLevelSet(const Grid& g, std::vector<double>& phi, const CellVelocityAt& velocity, double t, double dt, Pool* pool,
	const LevelSetBarrier* barrier = nullptr);

enum class ReinitScheme { None, Sussman, RussoSmereka, RussoSmerekaCentral, Subcell, Frozen };

//! `iterations` SSP-RK3 pseudo-time steps of dtau = cfl h of
//! phi_tau = -S(phi0) (|grad phi| - 1), S(phi0) = phi0 / sqrt(phi0^2 + h^2),
//! Godunov's Hamiltonian on WENO5 one-sided derivatives (Sussman).  Cells with
//! a sign change to a face neighbour ("near" cells) are treated by:
//!   RussoSmereka  relaxation to the distance estimate phi0 / |grad phi0|
//!                 (Russo and Smereka 2000, eq. 13; |grad phi0| from their
//!                 maximum of one-sided and central differences);
//!   RussoSmerekaCentral  the same relaxation with |grad phi0| from central
//!                 differences (the maximum only where the central estimate
//!                 falls below half of it: kinks, extrema), which is unbiased
//!                 for a stretched or curved smooth level set;
//!   Subcell       the interface as a boundary: the one-sided difference
//!                 across a sign change uses phi = 0 at the linear crossing
//!                 of phi0, phi/d (Chene, Min and Gibou 2008, with linear
//!                 instead of quadratic crossings), with a local pseudo-time
//!                 step min(cfl h, d/2);
//!   Frozen        near cells keep their values (the zero contour cannot
//!                 move; the rest is redistanced from them).
//! Measured choice (docs/SIMULATION_SOLVER_CONTRACT.md): RussoSmereka drifts when
//! applied every step (its estimate is biased high, so near-cell values
//! shrink call after call); Subcell is the default.
//! `barrier` (m7f): closed faces as walls (a neighbour across one is absent).
//! `wall` (m7g): the ghost values beyond the domain's walls in the pseudo-time
//! derivatives.  Linear (m7a) lets a wall cell's own deficit lie beyond the wall:
//! where |phi| falls towards a wall (phi_wall < phi_inner, phi > 0), the ghost is
//! smaller still, the Godunov Hamiltonian counts the wall-normal difference as an
//! upwind slope beside the slope from the true interface, |grad phi| > 1, and the
//! wall cell falls further: a false interface spreads along the wall, settling
//! with its zero on the wall plane (measured beside the walls below a sagging
//! sheet, 1.5% of the volume at 40 cells; docs/SIMULATION_SOLVER_CONTRACT.md).
//! Constant: zero normal slope at the walls (a 90 degree contact angle).
void Reinitialize(const Grid& g, std::vector<double>& phi, ReinitScheme scheme, int iterations, double cfl, Pool* pool,
	const LevelSetBarrier* barrier = nullptr, Extension wall = Extension::Linear);

//! The largest | |grad phi| - 1 | (central differences) over cells within
//! `band` cells of the interface: the trigger for reinitialisation.
double GradientDeviation(const Grid& g, const std::vector<double>& phi, double band, Pool* pool,
	const LevelSetBarrier* barrier = nullptr);

//! Smoothed Heaviside measures (epsilon = 1.5 h): the liquid volume
//! sum (1 - H(phi)) open h^3 and the interface area sum delta(phi) |grad phi| open h^3.
//! `open` (may be null) weights each cell (the open fraction outside solids).
double LiquidVolume(const Grid& g, const std::vector<double>& phi, const std::vector<double>* open, Pool* pool);
//! `exclude` (m7f; may be null): cells left out of the area.  `barrier` (m7l; may be null):
//! |grad phi| one-sided at its closed faces (a sheet's two sides are not differenced).
double InterfaceArea(const Grid& g, const std::vector<double>& phi, const std::vector<double>* open, Pool* pool,
	const std::vector<std::uint8_t>* exclude = nullptr, const LevelSetBarrier* barrier = nullptr);
//! Shifts phi by s = (V(phi) - target) / A so that the liquid volume returns
//! to `target` (one Newton step; the shift is returned: metres).
double CorrectVolume(const Grid& g, std::vector<double>& phi, double target, const std::vector<double>* open, Pool* pool);

//! Mean curvature (sum of the principal curvatures, 1/m) of the level sets
//! of phi at every cell centre, kappa = div(grad phi / |grad phi|), from
//! second-order central differences (m7c surface tension):
//!   kappa = [phi_x^2 (phi_yy + phi_zz) + phi_y^2 (phi_xx + phi_zz) + phi_z^2 (phi_xx + phi_yy)
//!            - 2 phi_x phi_y phi_xy - 2 phi_x phi_z phi_xz - 2 phi_y phi_z phi_yz] / |grad phi|^3.
//! Positive for a convex liquid region (phi < 0 inside: 1/R for a circle, 2/R
//! for a sphere).  At the domain walls the stencil mirrors phi (clamped
//! indices), which is zero normal slope: a 90 degree contact angle in the
//! curvature.  Clamped to |kappa| <= (dims - 1) / h (dims the axes with more
//! than one cell); zero where |grad phi| < 1e-6.
void Curvature(const Grid& g, const std::vector<double>& phi, std::vector<double>& kappa, Pool* pool);


// ------------------------------------------------------------------ marker particles

//! The particle level set (Enright et al. 2002): particles on both sides of
//! the interface within `band` cells, each with a sign, a radius in
//! [0.1 h, 0.5 h] and a stable id; escaped particles correct phi through
//! their spheres phi_p = s_p (r_p - |x - x_p|), positive particles raising
//! phi+ = max(phi+, phi_p) and negative ones lowering phi- = min(phi-, phi_p)
//! at the eight cell centres around them, phi = the one of smaller
//! magnitude.  Seeding and reseeding use a hash of (cell, step) for their
//! random numbers, so the set is reproducible.
struct ParticleOptions {
	int perCell = 16;        // particles seeded per band cell (3-D); thin grids use it as is
	double band = 3;         // seeding band, cells
	double rmin = 0.1, rmax = 0.5;   // radius bounds, cells
	int reseedInterval = 20; // steps
};

class MarkerParticles {
public:
	struct Particle { V3 x; double radius; double sign; std::uint64_t id; };
	void Seed(const Grid& g, const std::vector<double>& phi, const ParticleOptions& options, std::uint64_t step, Pool* pool);
	//! RK3 (Shu-Osher) with the MAC velocity from `velocityAt(time)`.
	void Advect(const Grid& g, const std::function<const MacField&(double)>& velocityAt, double t, double dt, Pool* pool);
	//! Corrects phi from the escaped particles; returns how many escaped.
	std::size_t Correct(const Grid& g, std::vector<double>& phi, Pool* pool) const;
	//! After reinitialisation: radii from phi, particles far on the wrong side deleted.
	void AdjustRadii(const Grid& g, const std::vector<double>& phi, Pool* pool);
	//! Adds particles to band cells below half the target count and deletes
	//! non-escaped particles outside the band (ids stay increasing).
	void Reseed(const Grid& g, const std::vector<double>& phi, std::uint64_t step, Pool* pool);
	const std::vector<Particle>& Particles() const { return particles_; }
	std::vector<Particle>& Particles() { return particles_; }
	std::uint64_t NextId() const { return nextId_; }
	void SetState(std::vector<Particle> particles, std::uint64_t nextId, const ParticleOptions& options) {
		particles_ = std::move(particles); nextId_ = nextId; options_ = options;
	}
	const ParticleOptions& Options() const { return options_; }
private:
	void SeedCells(const Grid& g, const std::vector<double>& phi, const std::vector<std::size_t>& cells,
		const std::vector<int>& counts, std::uint64_t step, std::vector<Particle>& out) const;
	std::vector<Particle> particles_;
	std::uint64_t nextId_ = 1;
	ParticleOptions options_;
};

} } }

#endif
