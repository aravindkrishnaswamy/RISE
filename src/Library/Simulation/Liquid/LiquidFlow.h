//////////////////////////////////////////////////////////////////////
//
//  LiquidFlow.h - Momentum and pressure of the liquid solver
//  (simulation M7): velocity extrapolation into the air, momentum
//  advection (three schemes, chosen by measurement), and the pressure
//  projection with a ghost-fluid free surface and cut-cell solids, solved
//  by multigrid-preconditioned conjugate gradients.
//
//  Pressure system.  The unknown is the scaled pressure q = p dt / (rho h)
//  (m/s) in every liquid cell (phi < 0) with an open face.  A face of axis
//  a between cells L and R with open fraction A (LiquidGrid.h FaceFractions)
//  carries u = u* - (q_R - q_L); the discrete divergence of a cell weights
//  each face by A and adds the solid's velocity on the closed part:
//      sum_f A_f (u*_f - grad_f q) . n_f + (1 - A_f) u_s,f . n_f = 0,
//  the variational (Batty, Bertails and Bridson 2007) form with face-area
//  weights (Ng, Min and Gibou 2009).  It is symmetric: the operator is
//  (A q)_c = sum_f A_f (q_c - q_nb).  A face to an air cell (phi >= 0) puts
//  p = 0 at the interface, at the fraction theta = phi_L / (phi_L - phi_R)
//  of the way from the liquid centre (Gibou, Fedkiw, Cheng and Kang 2002;
//  Enright, Nguyen, Gibou and Fedkiw 2003): the ghost value
//  q_R = -q_L (1 - theta) / theta adds A / theta to the diagonal only.
//  theta is clamped to [thetaMin, 1].  An open top boundary puts p = 0 on
//  the boundary face: theta = clamp(-phi_c / h, thetaMin, 1/2).
//
//  Rigid bodies (m7b, LiquidBodies.h).  Batty et al. section 4 couple a
//  rigid body by appending its six velocity unknowns; eliminating them
//  gives the symmetric positive semidefinite term rho h^3 J^T M^-1 J added
//  to the operator, with J the body's face-weighted velocity Jacobian.  The
//  operator below applies `couplings` after the grid stencil; the body's
//  predicted velocity enters the right-hand side through the solid face
//  velocities.  The preconditioner stays the grid's.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_FLOW_H
#define RISE_SIMULATION_LIQUID_FLOW_H

#include "LiquidGrid.h"

namespace RISE { namespace Simulation { namespace Liquid {

//! Valid (known) faces per axis.
using FaceMask = std::array<std::vector<std::uint8_t>,3>;

//! Fills unknown faces, layer by layer up to `layers`, with the mean over
//! their known face neighbours of the same component (a layer reads only
//! faces known before it) of: in the first `linearLayers` layers the linear
//! extrapolation 2 u_1 - u_2 along that axis when the second face is known
//! too, else u_1.  Faces never reached are set to zero.  (Constant
//! extrapolation alone made the surface velocity first-order accurate: the
//! standing-wave frequency converged at order one.)
void ExtrapolateVelocity(const Grid& g, MacField& u, FaceMask& valid, int layers, int linearLayers, Pool* pool);

enum class MomentumScheme { SemiLagrangian, MacCormack, Weno5 };

//! Advects every face component by the (extrapolated) field u over dt:
//!   SemiLagrangian  second-order Runge-Kutta backtrace, trilinear sampling;
//!   MacCormack      the semi-Lagrangian step corrected by half the error of a
//!                   backward step, limited to the range of the forward
//!                   step's interpolation stencil, else reverted (Selle,
//!                   Fedkiw, Kim, Liu and Rossignac 2008);
//!   Weno5           u_t + (u . grad) u = 0 per component on its own face
//!                   grid: WENO5 upwind derivatives, the advecting velocity
//!                   averaged to the face, SSP-RK3 (the advecting field
//!                   recomputed at each stage) of LimitedAdvectionStage.
//! Components along a one-cell axis stay zero.
void AdvectVelocity(const Grid& g, const MacField& u, double dt, MomentumScheme scheme, MacField& out, Pool* pool);
//! The semi-Lagrangian or MacCormack step of the field `f` along the
//! velocity `v` (a second stage advects u^n along the stage average).
void AdvectFieldAlong(const Grid& g, const MacField& f, const MacField& v, double dt, MomentumScheme scheme, MacField& out, Pool* pool);
//! rate = -(u . grad) u per face component (the Weno5 spatial operator).
void VelocityAdvectionRate(const Grid& g, const MacField& u, MacField& rate, Pool* pool);
//! One forward-Euler stage of the Weno5 operator, out = u + dt L(u), with
//! each face value then limited to the range of u over the corners of the
//! face's upwind cell (the face and its neighbours one step against the
//! advecting velocity along each axis): the exact transport of each
//! component takes its value from that cell when the CFL number is at most
//! one, so a stage creates no new extremum.  WENO5's one-sided derivatives
//! read two faces downwind; at a velocity peak against a wall in a film one
//! or two cells thick every candidate stencil is non-smooth and the
//! derivative took the wrong sign, so the unlimited stage raised the peak
//! by about 10% a step (docs/SIMULATION_M7A_RESULTS.md section 13).  In
//! smooth flow the bound is inactive except at extrema.
void LimitedAdvectionStage(const Grid& g, const MacField& u, double dt, MacField& out, Pool* pool);

//! A rigid body's degrees of freedom in the pressure solve (LiquidBodies.h
//! BodyCoupling): Project calls Begin with the active cells once they are
//! known, applies every coupling after the grid stencil in the operator,
//! lets each add to the right-hand side and calls Finish with the solution.
class IPressureCoupling {
public:
	virtual ~IPressureCoupling() = default;
	//! The liquid cells of the system (active[c] != 0), before any other call.
	virtual void Begin(const std::vector<std::uint8_t>& active) { (void)active; }
	//! out += (J M^-1 J^T) q over the cell-centred array (active cells only).
	virtual void ApplyAdd(const std::vector<double>& q, std::vector<double>& out) const = 0;
	//! b += the body's contribution (a body supplies its velocity through the
	//! solid face velocities instead, so this may do nothing).
	virtual void AddRhs(std::vector<double>& b) const = 0;
	//! After the solve: the body's impulse from q.
	virtual void Finish(const std::vector<double>& q) = 0;
};

//! The cells of the pressure system: liquid (phi < 0) with an open face.
void ActiveCells(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	std::vector<std::uint8_t>& active, Pool* pool);

struct PressureOptions {
	double tolerance = 1e-8;     // ||r||_inf <= tolerance ||b||_inf
	int maxIterations = 1000;
	double thetaMin = 1e-3;
	int smoothing = 2;           // red-black Gauss-Seidel sweeps before and after each coarse correction
	bool openTop = false;        // +y boundary: p = 0 there
	bool warmStart = false;      // start PCG from the q passed in (else from zero)
	double minValidFraction = 0; // updated cut faces less open than this are left invalid (re-extrapolated)
	//! Surface tension (m7c): the Laplace jump p_liquid - p_air = sigma kappa
	//! at the interface, imposed by the ghost fluid (Kang, Fedkiw and Liu
	//! 2000): a face from a liquid cell L to an air cell R puts q = q_I at the
	//! interface, q_I = jumpScale kappa_I, so the ghost value is
	//! q_L + (q_I - q_L) / theta: the operator is unchanged and the right-hand
	//! side gains A q_I / theta.  kappa_I is the cell-centred curvature
	//! interpolated to the interface, kappa_L + theta (kappa_R - kappa_L), or
	//! (interpolateCurvature false) the liquid cell's.  Null: no jump (the
	//! m7b arithmetic).  jumpScale = sigma dt w / (rho h), w the stage's
	//! weight in the step's pressure.  The open top keeps p = 0.
	const std::vector<double>* curvature = nullptr;
	double jumpScale = 0;
	bool interpolateCurvature = true;
};

struct PressureStats {
	int iterations = 0, levels = 0;
	double residual = 0;         // final ||r||_inf / ||b||_inf
	double seconds = 0;
	double assemble = 0, components = 0, hierarchy = 0, solve = 0, update = 0;   // wall seconds by phase
	std::size_t unknowns = 0;
	bool converged = false;
};

//! Projects u (on input u*, on output divergence-free on every liquid cell):
//! builds the system from phi and the face open fractions, solves it, and
//! updates every face with an open part next to a liquid cell.  Those faces
//! are valid when their open fraction A is at least options.minValidFraction;
//! narrower cut faces carry the velocity that satisfies their cells'
//! constraints but are left invalid, so the extrapolation replaces them
//! before the velocity is transported.  Such a face's velocity is the speed
//! in its open part, A u its flux: copied into a full face by the
//! extrapolation or the advection it carried 1/A times the flux, which the
//! next projection drained through the nearly empty cells beside the solid
//! at u* + q / theta, as jets (docs/SIMULATION_M7A_RESULTS.md section 13).
//! The projection minimises sum A_f theta_f (u_f - u*_f)^2 (theta = 1
//! between liquid cells; the ghost-fluid update is that minimiser).  Faces with no open part
//! take the solid velocity (zero when `solid` is null); domain boundary
//! faces are valid, others invalid (to extrapolate).  `q` receives the
//! scaled pressure (0 outside the liquid).
bool Project(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const MacField* solid, const PressureOptions& options, const std::vector<IPressureCoupling*>& couplings,
	MacField& u, std::vector<double>& q, FaceMask& valid, PressureStats& stats, Pool* pool, std::string& error);

//! u -= scale G q on every face Project updates (an open part next to a liquid
//! cell of the system phi and the fractions give), G Project's gradient
//! (q_R - q_L between liquid cells; (q_L - q_I) / theta to the air, with the
//! surface-tension jump of `options`).  m7c: the incremental pressure of the
//! viscous step (LiquidModel.cpp), which subtracts the previous pressure
//! gradient before the viscous solve and adds it back before the projection.
//! `interiorOnly`: only the faces between two liquid cells.
void SubtractPressureGradient(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const PressureOptions& options, const std::vector<double>& q, double scale, MacField& u, Pool* pool, bool interiorOnly = false);

//! The operator of Project's system for tests: y = (A + sum of the
//! couplings' terms) x on the active cells (zero elsewhere), after the same
//! assembly; `active` receives the cells.  Couplings get Begin first.
void ApplyPressureOperator(const Grid& g, const std::vector<double>& phi, const std::array<std::vector<double>,3>& fraction,
	const PressureOptions& options, const std::vector<IPressureCoupling*>& couplings, const std::vector<double>& x,
	std::vector<double>& y, std::vector<std::uint8_t>& active, Pool* pool);

} } }

#endif
