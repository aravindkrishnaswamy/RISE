//////////////////////////////////////////////////////////////////////
//
//  LiquidViscosity.h - Implicit constant viscosity of the liquid solver
//  (simulation M7c, docs/SIMULATION_M7C_RESULTS.md).
//
//  Formulation (derived from the variational principle; the form of Batty
//  and Bridson 2008, "Accurate viscous free surfaces for buckling, coiling,
//  and rotating liquids").  Backward Euler for rho u_t = div(2 mu D(u)),
//  D = (grad u + grad u^T) / 2, is the minimiser of
//      E(u) = (rho h^3 / 2) sum_f m_f (u_f - u*_f)^2
//           + dt mu h^3 [ sum_cells w_c sum_a D_aa^2 + 2 sum_edges w_e D_ab^2 ],
//  since dE/du = 0 is rho (u - u*) = dt div(2 mu D) in the interior.  The
//  diagonal strains D_aa = d_a u_a live at cell centres, the shear strains
//  D_ab = (d_b u_a + d_a u_b) / 2 at the cell edges parallel to the third
//  axis; each is weighted by the liquid fraction of its control volume (the
//  cube of side h centred on it) outside the solids, w_c and w_e (4^3
//  samples of the level set and the solid distance in a cut volume).  Where the weights
//  vanish (air) the strain carries no energy, so the free surface is the
//  natural boundary condition of the minimisation: zero viscous traction
//  2 mu D n = 0 (Batty and Bridson's point: a plain Laplacian with Neumann
//  conditions puts grad u n = 0 instead, which spins a rotating blob down).
//  The face masses m_f are the projection's kinetic-energy weights (A between
//  liquid cells, A theta to the air; LiquidFlow.h Project), floored at
//  massFloor on faces only the strains reach, so the viscous and pressure
//  steps measure momentum alike.  The system is
//      (M + 2 dt nu G^T S G) u = M u* - 2 dt nu G^T S g0,
//  G the strains' dependence on the unknown faces, g0 their constant part,
//  S = (w_c, 2 w_e): symmetric positive definite.  It is solved by
//  Jacobi-preconditioned conjugate gradients with fixed-order reductions
//  (bit-identical for any thread count).
//
//  Boundaries.  No slip (default): a face whose centre lies inside a solid
//  (static or a body) is fixed at the solid's velocity; a difference
//  between a liquid face and a fixed one puts the solid's velocity at the
//  wall crossing, a fraction theta = phi_s,1 / (phi_s,1 - phi_s,2) of the
//  way (the ghost-fluid Dirichlet condition, theta clamped to thetaMin), the
//  rigid velocity of the body at that point; a tangential difference
//  across a domain wall puts zero at the wall (theta = 1/2).  Free slip:
//  solid faces are unknowns like the rest (the open-fraction weights make
//  the solid a traction-free surface for this step; the pressure step keeps
//  the normal velocity), and the shear strains on the domain walls are
//  dropped.  The normal velocity on the domain walls is zero in both.  An
//  open top is traction free.
//
//  Forces.  By translation invariance of the strain energy, the liquid's
//  momentum change in the step is sum over the boundary values v of
//  dE_strain/dv: the impulse of each body, of the static solids and of the
//  domain walls on the liquid.  Kinematic bodies receive the negative (with
//  its moment about their centre).  Bodies the liquid integrates are
//  unknowns of the solve (ViscousTwist::coupled): their wall values are their
//  rigid velocity at the crossing, linear in (V, Omega), and their kinetic
//  energy joins E, so the coupling is implicit and the system stays SPD; a
//  lagged version (the body held during the solve, its impulse applied after)
//  diverged below a density ratio of about 0.5 (docs, section 3).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_VISCOSITY_H
#define RISE_SIMULATION_LIQUID_VISCOSITY_H

#include "LiquidFlow.h"

#include <functional>
#include <string>

namespace RISE { namespace Simulation { namespace Liquid {

struct ViscosityOptions {
	double kinematic = 0;        // nu = mu / rho (m^2/s)
	bool noSlip = true;          // solids and domain walls (else free slip)
	bool openTop = false;        // +y boundary open: traction free
	double tolerance = 1e-8;     // ||r||_inf <= tolerance ||b||_inf
	int maxIterations = 20000;
	double thetaMin = 0.05;      // smallest wall fraction of a no-slip difference
	double massThetaMin = 1e-3;  // the pressure's theta_min (face masses)
	double massFloor = 1e-6;     // mass of unknown faces with no kinetic weight
	int cvSamples = 4;           // samples per axis of a cut control volume (its liquid and open fractions)
};

//! A rigid body's motion for the no-slip wall velocity: centre, velocity,
//! angular velocity.  `coupled`: the body's free degrees of freedom (`free`:
//! x, y, z, rotation) are unknowns of the viscous solve with its kinetic
//! energy (1/2)(w - w*)^T M (w - w*) added to E, M = diag(mass, inertia (world,
//! row-major)), w* = (v, w): an implicit viscous coupling (the system stays
//! symmetric positive definite); its impulse is M (w - w*).
struct ViscousTwist {
	V3 x{{0, 0, 0}}, v{{0, 0, 0}}, w{{0, 0, 0}};
	bool coupled = false;
	double mass = 0;
	std::array<double,9> inertia{{0, 0, 0, 0, 0, 0, 0, 0, 0}};
	std::array<bool,4> free{{true, true, true, true}};
};

//! m7m: a point of a sheet (a crossing): the crossed triangle's vertices and the barycentric weights.
struct ViscousSheetPoint {
	std::uint32_t sheet = 0;
	std::uint32_t v[3] = {0, 0, 0};
	double w[3] = {0, 0, 0};
};
//! m7m: a face a sheet closes (a barrier of the pressure): a no-slip wall of the viscous solve
//! at the crossing, coordinate s along the face's axis.
struct ViscousSheetFace {
	int axis = 0;
	std::size_t face = 0;
	double s = 0;
	ViscousSheetPoint p;
};
//! m7m: a difference of component `comp` along `dir` (dir != comp) between the faces of comp at
//! `hi` - e_dir and `hi` (index triple on comp's face grid) whose segment a sheet crosses, a
//! fraction t of the way from the low face; `loSide` the sheet side (+1 front, -1 back: the
//! triangle's normal) of the low face.
struct ViscousSheetCut {
	int comp = 0, dir = 0;
	int hi[3] = {0, 0, 0};
	double t = 0.5;
	int loSide = -1;
	double normal[3] = {0, 0, 0};
	ViscousSheetPoint p;
};
//! m7m: a sheet's velocity in the viscous solve.  `coupled`: its free vertex velocities V are
//! unknowns with the kinetic energy (1/2)(V - V*)^T S' (V - V*), S' the sheet's step matrix with
//! the face liquid (LiquidSheets.h SheetDynamics), V* = v; else (and for fixed vertices) v is given.
struct ViscousSheet {
	std::vector<double> v;                 // 3 per vertex (m/s)
	std::vector<std::uint8_t> fixed;       // per vertex: given (a pin); empty: none
	bool coupled = false;
	//! y = S' x (3 per vertex, N s/m times m/s; zero on fixed vertices).
	std::function<void(const std::vector<double>&, std::vector<double>&)> applyMass;
	//! Prepares the preconditioner (S' + diag(d))^-1, d 3 per vertex (N s/m).
	std::function<bool(const std::vector<double>&, std::string&)> prepare;
	//! z = (S' + diag(d))^-1 r (zero on fixed vertices), after prepare.
	std::function<void(const std::vector<double>&, std::vector<double>&)> precondition;
};

struct ViscosityInput {
	const std::vector<double>* solidNodes = nullptr;   // node signed distance of the solids (null: none)
	const MacField* solid = nullptr;                   // solid face velocities (bodies; null: zero)
	//! Per face: the body that closes it (-1: the static solids); null: all static.
	const std::array<std::vector<std::int16_t>,3>* owner = nullptr;
	const std::vector<ViscousTwist>* bodies = nullptr;
	//! m7m: sheets (null or empty: none).  The faces they close are no-slip walls at the crossing
	//! (always, whatever `noSlip` says of the walls and solids); a difference whose segment crosses a
	//! sheet is one-sided on each side, the sheet's velocity at the crossing (ghost fluid, the
	//! fraction to the crossing), so no strain couples the liquid across a sheet; a strain at a cell
	//! edge whose differences a sheet cuts is split into one per side, each weighted by its side's
	//! part of the edge's control volume (LiquidViscosity.cpp).
	const std::vector<ViscousSheet>* sheets = nullptr;
	const std::vector<ViscousSheetFace>* sheetFaces = nullptr;
	const std::vector<ViscousSheetCut>* sheetCuts = nullptr;
};

struct ViscosityStats {
	int iterations = 0;
	double residual = 0, seconds = 0, build = 0;   // build: the weights, kinds and diagonal
	std::size_t unknowns = 0;
	bool converged = false;
};

using Impulse6 = std::array<double,6>;

//! One implicit viscous step of length dt on u (u* on input).  bodyImpulse
//! (one per body of input.bodies) receives the impulse on each body (force
//! dt; torque dt about its centre; for a coupled body M (w - w*)), wallImpulse
//! the impulse of the static solids and the domain walls on the liquid.  Faces that are not unknowns
//! keep their input values.
//! m7m: `sheetImpulse` (may be null; one per input sheet, 3 per vertex, N s) receives the viscous
//! impulse on each sheet: S' (V - V*) for a coupled one, the reaction of the strains on its
//! given velocities otherwise (zero on fixed vertices of a coupled sheet: pins absorb theirs).
bool ViscousStep(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const ViscosityInput& input, const ViscosityOptions& options, double dt, double density, MacField& u,
	std::vector<Impulse6>& bodyImpulse, V3& wallImpulse, ViscosityStats& stats, Pool* pool, std::string& error,
	std::vector<std::vector<double>>* sheetImpulse = nullptr);

//! For tests: y = (M + 2 dt nu G^T S G) x on the unknown faces (zero on the
//! others) after the same assembly; `unknown` receives the unknown mask.
void ApplyViscousOperator(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const ViscosityInput& input, const ViscosityOptions& options, double dt, const MacField& x, MacField& y,
	FaceMask& unknown, Pool* pool);

} } }

#endif
