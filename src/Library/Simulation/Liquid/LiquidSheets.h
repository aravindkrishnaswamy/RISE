//////////////////////////////////////////////////////////////////////
//
//  LiquidSheets.h - Thin open surfaces (a cloth, a paddle) as moving
//  boundaries of the liquid solver (simulation M7d, preview;
//  docs/SIMULATION_M7D_RESULTS.md).
//
//  A zero-thickness sheet cannot be a signed-distance solid.  It is a
//  face-crossing barrier instead (Guendelman, Selle, Losasso and Fedkiw 2005, in
//  form): a face of the staggered grid is closed when the segment between
//  the centres of the two cells it separates crosses a sheet triangle.  No
//  volume is removed (the cell fractions are unchanged); flow through the
//  sheet is blocked because every path of neighbouring cells from one side to
//  the other crosses such a segment.  A closed face carries the sheet's
//  normal velocity there, (v . n) n_a (v the barycentric velocity at the
//  crossing, n the triangle's unit normal), as a solid face velocity, so a
//  moving sheet pushes the liquid without pumping it along itself.  The
//  sheet is located to the cell-centre lattice: a staircase of half a cell.
//
//  Crossings.  For each triangle and axis a, the lattice lines along a
//  through the cell centres of the other two axes are intersected with the
//  triangle (barycentric coordinates in the projection, inclusive by 1e-10 so
//  that a line through a shared edge or vertex is never missed); the crossing
//  at coordinate s closes the face between the two cell centres that bracket
//  s.  A face crossed more than once (shared edges, folds, two sheets) keeps
//  the crossing nearest its centre, ties by sheet and triangle: the result is
//  independent of the thread count.
//
//  Load.  A closed face f of axis a between cells L and R (L on the minus
//  side) receives the impulse rho h^3 (q_L - q_R) e_a in a step (q the step's
//  scaled pressure, 0 in air): the counterpart of LiquidBodies.h's rho h^3 J q
//  for a fully closed face, so the liquid's impulse on the sheet is minus the
//  sheet's on the liquid.  It is spread to the crossed triangle's vertices by
//  the barycentric weights of the crossing.
//
//  Implicit coupling (m7e, docs/SIMULATION_M7E_RESULTS.md).  A port sheet
//  with `coupling: implicit` carries its partner's linearised response for
//  the macro step (SheetDynamics: lumped masses M, pins, the force f0 at the
//  macro step's start x0, the projected elastic Hessian K, Rayleigh
//  coefficients alpha and beta).  In each step of length dt its vertex
//  velocities V solve, with the pressure, the minimisation of
//      (rho h^3 / 2) sum_f A_f theta_f (u_f - u*_f)^2 + (1/2) (V - V*)^T S (V - V*),
//      S = M (1 + dt alpha) + (dt beta + dt^2) K,
//      V* = S^-1 (M V_n + dt (f0 - K (xe - x0)))      (backward Euler, linearised about x0),
//  subject to the discrete divergence, in which a closed face f carries
//  u_s,f = G_f V (the crossing's barycentric velocity along the triangle's
//  normal, (w . V) n_a n).  The second term is the sheet's incremental
//  potential of that step, so the minimisation is the monolithic step of the
//  liquid and the linearised sheet, derived as M7b's body coupling with S in
//  place of the body's mass matrix:
//      V = V* + rho h^3 S^-1 J q,   (A + rho h^3 J^T S^-1 J) q = b,
//      J_c = sum_{f of c closed by the sheet} s_cf G_f^T,
//  symmetric positive semidefinite (S is positive definite).  The impulse on
//  the sheet is rho h^3 J q: on each vertex of the crossed triangle
//  rho h^3 (q_L - q_R) w_m n_a n.  The sheet's added mass is therefore
//  implicit in every projection (the added-mass instability of the lagged
//  exchange cannot arise), and so is its elastic response to the pressure.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_SHEETS_H
#define RISE_SIMULATION_LIQUID_SHEETS_H

#include "LiquidBodies.h"
#include "../Cloth/ClothLinearAlgebra.h"

#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Liquid {

//! m7e: a 3x3 block (i <= j) added to an implicitly coupled sheet's step matrix.
struct SheetBlock {
	std::uint32_t i = 0, j = 0;
	double b[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
};

//! m7e: the linearised response of an implicitly coupled sheet over a macro
//! step (LiquidSheets.h header), and the factorisation of its step matrix.
struct SheetDynamics {
	std::vector<double> mass;          // per vertex, kg
	std::vector<std::uint8_t> fixed;   // per vertex: the velocity is prescribed (a pin)
	std::vector<double> force;         // 3 per vertex, N: the non-hydrodynamic force at x0
	std::vector<double> x0, xe;        // 3 per vertex: the linearisation point; the elastic positions (x0 + sum dt V)
	double dampingMass = 0, dampingStiffness = 0;   // Rayleigh alpha (1/s), beta (s)
	Cloth::BlockMatrix stiffness;      // K (3x3 blocks, symmetric pattern)
	std::string stiffnessDomain;       // the accepted pattern's index-space digest
	bool useStiffness = true;          // false: S = M (1 + dt alpha), f0 explicit (the measured contrast)
	// m7h: the corotational model (cloth's surface_elements; empty: m7e's K about x0).  With it,
	// K is sum R_e K_e R_e^T at the step's rotations and the elastic force is the model's,
	// -sum R_e (g_e + K_e (R_e^T xe - x0)), in place of -(g0 + K (xe - x0)); `force` keeps the
	// force at x0 (so the non-elastic part is force + sum g_e).
	Cloth::CorotatedElements elements;
	std::string elementsDomain;        // the accepted elements' index-space digest
	std::vector<double> origin;        // sum g_e (3 per vertex): the model's elastic gradient at x0
	double rotationLag = 0;            // the largest rotation lag of a step (rad; LiquidModel)
	// Not state: rebuilt every step.
	Cloth::BlockMatrix rotated;        // m7h: sum R_e K_e R_e^T at the step's rotations
	Cloth::BlockMatrix matrix;         // S plus the step's face mass, pins masked
	Cloth::BandedCholesky factor;
	std::vector<std::uint32_t> order;
	std::vector<double> pinned;        // (S + F)_fp V_p (free rows)
	bool Active() const { return !mass.empty(); }
	std::size_t Vertices() const { return mass.size(); }
	bool WithStiffness() const { return useStiffness && stiffness.Blocks() == mass.size(); }
	//! m7h: the corotational model is in use.
	bool Corotated() const { return WithStiffness() && !elements.Empty(); }
	//! The step's stiffness: sum R_e K_e R_e^T (corotated) or K.
	const Cloth::BlockMatrix& Stiffness() const { return Corotated() ? rotated : stiffness; }
	//! m7h: the elements' rotations for a step of length dt from the predicted positions
	//! xe + dt v (as cloth.shell sets its own from x + h v), and the step's stiffness.
	//! Before Prepare; nothing without the corotational model.
	void Rotate(double dt, const std::vector<double>& v);
	//! m7h: with `elements` filled (accepted or restored) and x0 set: rotations the identity,
	//! `origin` and the stiffness at x0.
	void ResetElements();
	//! Factors S + F for dt, F the blocks `extra` (the face mass), the pattern the
	//! stiffness's and the triangles' pairs.  `v`: the current velocities (the pins').
	bool Prepare(double dt, const std::vector<double>& v, const std::vector<SheetBlock>& extra,
		const std::vector<std::uint32_t>& triangles, std::string& error);
	//! out = (S + F)^-1 r with the pinned components zero (after Prepare).
	void Solve(const std::vector<double>& r, std::vector<double>& out) const;
	//! The sheet's own right-hand side M v + dt (f0 - K (xe - x0)).
	void Rhs(double dt, const std::vector<double>& v, std::vector<double>& out) const;
	//! The predictor (S + F)^-1 (Rhs + extra - (S + F)_fp V_p), pins keeping their velocity.
	void Predict(double dt, const std::vector<double>& v, const std::vector<double>& extra, std::vector<double>& out) const;
	//! out = S V (the sheet's own step matrix, no face mass, pins unmasked).
	void ApplyS(double dt, const std::vector<double>& V, std::vector<double>& out) const;
};

struct LiquidSheet {
	std::string name;
	enum class Motion {
		Prescribed,   // a rigid motion of `local` (LiquidBody::PrescribedAt of `path`)
		Port          // positions and velocities from a mesh.v2 surface (cloth's surface_state) at each macro start, drifted with that velocity
	};
	Motion motion = Motion::Prescribed;
	std::vector<std::uint32_t> triangles;   // fixed topology
	std::vector<std::uint8_t> open;         // m7h, validation only (empty in the solver): per triangle, 1 for one that closes no face (a hole in the barrier)
	std::vector<double> x, v;               // world positions (m) and velocities (m/s), 3 per vertex
	// Prescribed: vertices in the sheet's frame and the frame's motion.
	std::vector<double> local;
	LiquidBody path;
	// Port: the accepted surface and the offered load.
	std::string port, loadPort;
	std::string topology;                   // the accepted surface's topology digest (the load's domain)
	// m7e: implicit coupling (its partner's mechanics and stiffness through two more ports).
	bool implicit = false;
	std::string mechanicsPort, stiffnessPort, stepsPort;
	std::string elementsPort;               // m7h: cloth's surface_elements (the corotational model)
	SheetDynamics dyn;
	bool Coupled() const { return implicit && dyn.Active(); }
	std::size_t Vertices() const { return x.size()/3; }
	//! Prescribed: x and v at time t.
	void PrescribedAt(double t);
	//! A bound of the sheet's surface speed over the coming steps.
	double SpeedBound() const;
};

//! A face closed by a sheet.
struct SheetCrossing {
	int axis = 0;
	std::size_t face = 0;
	std::uint32_t sheet = 0, triangle = 0;
	double w[3] = {0, 0, 0};    // barycentric weights of the crossing on the triangle's vertices
	double distance = 0;        // |s - face coordinate| (the choice among several crossings)
	double velocity = 0;        // (v . n) n_a at the crossing
	double normal[3] = {0, 0, 0};   // m7e: the triangle's unit normal
	double s = 0;                   // m7e: the crossing's coordinate along the axis (world)
	// m7i: the part of the face's square the sheet covers (implicitly coupled sheets with a free
	// edge; 1 elsewhere).  A face c < 1 is split as a body's cut face: the fraction A (1 - c) stays
	// open liquid, the part c moves with the sheet (its J and face mass scaled by c); the level
	// set's barrier, the pin and the sided guards treat it as closed for c >= 1/2 only.
	double cover = 1;
	bool crossed = true;            // m7i: false for a face beside a free edge that the sheet does not cross (cover < 1)
	bool Barrier() const { return cover >= 0.5; }
};


//! Every face the sheets close (sorted by axis and face index; one entry per face).
//! m7i: for an implicitly coupled sheet (with its dynamics) that has a free edge (a boundary
//! edge not pinned at both ends), each face whose square the projection of a free edge reaches
//! gets its cover: the area of the sheet's projection along the face's axis inside the square
//! (the triangles reached from the crossed one through shared vertices whose projection meets
//! the square), over h^2, at most 1; 1 above 1 - 1e-9 and for every other face.  And it adds,
//! for each lattice line a free edge's projection meets whose face holding the edge's nearest
//! point is not crossed, that face (`crossed` false) with its cover, the edge's point as the
//! crossing (weights, coordinate, velocity) and the edge's triangle's normal.
void FindSheetCrossings(const Grid& g, const std::vector<LiquidSheet>& sheets, std::vector<SheetCrossing>& out, Pool* pool);
//! m7i: sets the cover of `faces` (FindSheetCrossings calls it).
void SheetCover(const Grid& g, const std::vector<LiquidSheet>& sheets, std::vector<SheetCrossing>& faces, Pool* pool);

//! The level set beside the sheets after a step (`old`: before it): an air
//! cell (old >= 0) beside a closed face whose level set fell keeps its old
//! value unless a neighbour across an open face (fraction > 0) was liquid
//! before the step.  Liquid reaches a cell through its faces; a cell beside a sheet that
//! fills without a wet open neighbour was filled across the sheet by the level
//! set's own stencils (the false interface where a sheet holds liquid on one
//! side only: a tilted sheet leaked 3% of the liquid in 4 s at 32 cells).
//! Without the creep check, those cells' level set crept towards zero and the
//! smoothed volume measure counted them as half full.  Returns the number of
//! cells held.
//! Anywhere in the grid (with sheets only), an air cell that turned liquid
//! without such a neighbour keeps its old value too: on the dry side of a
//! sheet the extrapolated velocity carries the false interface's level set
//! (near zero) away from the sheet (a 3-D sloshing tank wetted two dry cells
//! six cells from the sheet in 1 s without this).
std::size_t BlockSheetLeaks(const Grid& g, const std::vector<SheetCrossing>& faces, const std::array<std::vector<double>,3>& fraction,
	const std::vector<double>& old, std::vector<double>& phi, Pool* pool);

//! Whether a point that moved from `from` to `to` (less than a cell along each
//! axis) crossed the barrier: its cell changed and every axis-by-axis path of
//! neighbouring cells between the two cells crosses a closed face (m7i: one
//! with cover >= 1/2; `faces`
//! sorted by axis and face, as FindSheetCrossings gives them).  Points outside
//! the domain count as in the nearest cell.
bool CrossesSheets(const Grid& g, const std::vector<SheetCrossing>& faces, const V3& from, const V3& to);

//! m7e: the coupling of the implicitly coupled sheets in one projection
//! (LiquidSheets.h header): J over the faces those sheets close, S^-1 per
//! sheet (SheetDynamics, prepared for the step), and the impulse rho h^3 J q.
//! Every sum runs in the crossings' order on one thread: the result does not
//! depend on the thread count.
class SheetCoupling final : public IPressureCoupling {
public:
	//! `skip` (m7g; may be null): per crossing, 1 for a face left out (opened: the liquid does not reach the sheet there).
	SheetCoupling(const Grid& g, const std::vector<SheetCrossing>& faces, const std::vector<LiquidSheet>& sheets, double density,
		const std::vector<std::uint8_t>* skip = nullptr);
	void Begin(const std::vector<std::uint8_t>& active) override;
	void ApplyAdd(const std::vector<double>& q, std::vector<double>& out) const override;
	void AddRhs(std::vector<double>&) const override {}
	void Finish(const std::vector<double>& q) override;
	//! rho h^3 J q of the last Finish, per sheet (3 per vertex, N s; empty for sheets not coupled).
	const std::vector<std::vector<double>>& Impulse() const { return impulse_; }
	//! rho h^3 J q for any q (after Begin).
	void Measure(const std::vector<double>& q, std::vector<std::vector<double>>& out) const;
	bool Empty() const { return faces_.empty(); }
private:
	struct Face {
		std::size_t L = 0, R = 0;   // the cells on the minus and plus side
		bool aL = false, aR = false;
		std::uint32_t sheet = 0;
		std::uint32_t v[3] = {0, 0, 0};
		double c[3][3] = {{0}};     // per vertex m: w_m n_a n
	};
	const Grid& g_;
	const std::vector<SheetCrossing>& crossings_;
	const std::vector<LiquidSheet>& sheets_;
	const std::vector<std::uint8_t>* skip_ = nullptr;
	double scale_ = 0;              // rho h^3
	std::vector<Face> faces_;
	std::vector<std::vector<double>> impulse_;
	mutable std::vector<std::vector<double>> r_, z_;
};

//! m7e: sets the crossing velocity (V . n) n_a of every face an implicitly
//! coupled sheet closes from the velocities `v` (per sheet, 3 per vertex; an
//! empty array keeps the crossing's velocity).
void SheetCrossingVelocities(const std::vector<LiquidSheet>& sheets, const std::vector<std::vector<double>>& v,
	std::vector<SheetCrossing>& faces);

//! The per-vertex pressure (Pa) that, applied as cloth applies `surface_load`
//! pressure (force -p times the vertex's vector area: a third of each
//! adjacent triangle's area times its unit normal, from `x`), has the
//! component along that vector area of the force `impulse / span` (3 per
//! vertex, N s).  The tangential part is dropped (a pressure has none; on
//! the staircase it is discretisation noise).
void SheetPressure(const LiquidSheet& sheet, const std::vector<double>& impulse, double span, std::vector<double>& pressure);

} } }

#endif
