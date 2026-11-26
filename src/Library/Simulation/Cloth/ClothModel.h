//////////////////////////////////////////////////////////////////////
//
//  ClothModel.h - The cloth model of simulation M3a: a structured sheet,
//  its energies and the implicit time integrators
//
//  Sheet: a rectangle in material coordinates (u = warp, v = weft) meshed
//  as a regular grid of nu x nv vertices, each cell split into two right
//  triangles.  Triangulation "alternating" flips the diagonal in a
//  checkerboard (the union-jack pattern); "consistent" uses the same
//  diagonal everywhere.  The choice matters for bending (see the hinge
//  calibration below and docs/SIMULATION_M3A_RESULTS.md).
//
//  Energies (all in material coordinates, thickness-integrated):
//   * Membrane: orthotropic St. Venant-Kirchhoff on constant-strain
//     triangles.  F = Ds Dm^-1 (3x2), Green strain E = (F^T F - I)/2,
//     W = A0/2 e^T C e with e = (E11, E22, 2 E12) and the plane-stress
//     orthotropic C from E_warp, E_weft, G, nu_12.
//   * Bending, two discretisations on the structured mesh (hu, hv the cell
//     sides), both calibrated so that a quadratic surface
//     w = (a u^2 + 2 b u v + c v^2)/2 has energy density
//     (B_warp a^2 + B_weft c^2 + 2 sqrt(B_warp B_weft) b^2)/2, i.e. an
//     orthotropic plate with D11 = B_warp, D22 = B_weft, D66 =
//     sqrt(B_warp B_weft)/2 and D12 = 0 (no Poisson coupling in bending):
//     - "lines" (default): every vertex row and column is a discrete rod,
//       E = k |kappa b|^2 per interior vertex of the line with the curvature
//       binormal kappa b = 2 e1 x e2 / (|e1||e2| + e1.e2) (Bergou et al. 2008;
//       |kappa b| = 2 tan(psi/2), psi the turning angle), k = B_warp w_j/(2 hu)
//       along rows (w_j the row's share of the width: hv, hv/2 on the edges)
//       and B_weft w_i/(2 hv) along columns; twist from the diagonal hinges
//       below.  Rows and columns see total (normal plus in-plane) curvature.
//     - "hinges": discrete-shell hinges on every interior edge, E = k theta^2
//       (theta the dihedral angle), k = B_warp hv/(2 hu) on edges along v,
//       B_weft hu/(2 hv) on edges along u.  Consistent but not stable on
//       either structured triangulation (a grid-scale mode between rows
//       softens it; docs/SIMULATION_M3A_RESULTS.md); kept for the record.
//     Diagonal hinges (both models): k = sqrt(B_warp B_weft) hu hv/(hu^2+hv^2).
//     Clamp correction: an element one of whose wings is entirely pinned
//     represents curvature over half a cell and gets twice the stiffness.
//     Gradients exact; Hessians Gauss-Newton (2k J^T J).
//   * Lumped mass: areal density times the vertex's tensor-product
//     trapezoid area (hu hv inside, halves on edges, quarters at corners).
//   * Gravity, a constant or per-vertex pressure (follower, frozen per step),
//     per-vertex traction, optional quadratic air drag (explicit, frozen per
//     step), Rayleigh damping D = alpha M + beta K(x_n) (calibrated).
//   * Contact: a C2 penalty in the depth d = thickness - signed distance
//     against planes, spheres and axis-aligned boxes, stiffness k = k_c A_i:
//     k d^3/(6 delta) for d < delta = thickness/2, then quadratic
//     (k d^2/2 - k delta d/2 + k delta^2/6).  Interactive quality only: no
//     continuous collision detection, no friction.
//   * Pins: Dirichlet vertices, fixed or following a prescribed rigid motion.
//
//  Time integration minimises an incremental potential per step,
//    Phi(x) = |x - xhat|^2_M / (2 hhat^2) + E(x) - f.(x - x_n) + c/2 |x - xtilde|^2_D,
//  by Newton with a backtracking line search.  Each solve first uses the
//  exact membrane Hessian (the inertia term usually keeps the total positive
//  definite); when the linear solver finds it indefinite or the direction is
//  not a descent direction it switches to the projected Hessian (the
//  membrane's stress in the geometric term clamped to its positive part).
//  Bending Hessians are Gauss-Newton and contact drops its curvature term in
//  both.  The gradient is always exact, so converged solutions are those of
//  the unmodified problem; the modifications change only the path.  Backward Euler, BDF2 with
//  variable steps (backward-Euler first step) and implicit midpoint share
//  this machinery.  A step that does not converge is retried as two half
//  steps (variable-step BDF2 keeps second order across the change).
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_MODEL_H
#define RISE_SIMULATION_CLOTH_MODEL_H

#include "ClothMaterial.h"
#include "ClothLinearAlgebra.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

using V3 = std::array<double,3>;

enum class Triangulation { Alternating, Consistent };
enum class BendingModel { Lines, Hinges };
enum class Scheme { BackwardEuler, BDF2, ImplicitMidpoint };
enum class LinearSolver { Auto, ConjugateGradient, Direct, MultigridCG };
const char* TriangulationName(Triangulation t);
const char* BendingModelName(BendingModel b);
const char* SchemeName(Scheme s);
const char* LinearSolverName(LinearSolver s);

class BarrierContact;
struct ContactFrame;

struct SheetSpec {
	double sizeU = 1, sizeV = 1;                 // rest size in material coordinates, m
	std::uint32_t verticesU = 51, verticesV = 51;
	Triangulation triangulation = Triangulation::Alternating;
	BendingModel bending = BendingModel::Lines;
	V3 origin{{0, 0, 0}};
	V3 axisU{{1, 0, 0}}, axisV{{0, 0, -1}};      // world directions of u and v (orthonormal)
	double stretchU = 1, stretchV = 1;           // initial placement stretch (prestress when pinned)
};

enum class HingeKind : std::uint8_t { Warp, Weft, Diagonal };
struct Hinge {
	std::uint32_t v[4];  // edge x0,x1; x2 opposite in (x0,x1,x2); x3 opposite in (x1,x0,x3)
	double stiffness = 0;
	HingeKind kind = HingeKind::Diagonal;
};
struct Rod {
	std::uint32_t v[3];  // consecutive vertices of a row (Warp) or column (Weft)
	double stiffness = 0;
	HingeKind kind = HingeKind::Warp;
};
struct Triangle {
	std::uint32_t v[3];  // counter-clockwise in (u, v)
	double dmInv[4];     // inverse of [X1-X0, X2-X0] in material coordinates, row-major
	double area = 0;     // rest area, m^2
};

struct Sheet {
	SheetSpec spec;
	double hu = 0, hv = 0;
	std::vector<double> materialU, materialV; // per vertex, m
	std::vector<Triangle> triangles;
	std::vector<Hinge> hinges;
	std::vector<Rod> rods;
	std::vector<double> mass, area;           // per vertex: kg, rest dual area m^2
	std::vector<double> initial;              // 3 per vertex, world positions
	std::uint32_t Vertices() const { return spec.verticesU*spec.verticesV; }
	std::uint32_t Index(std::uint32_t i, std::uint32_t j) const { return i + spec.verticesU*j; }
};
bool BuildSheet(const SheetSpec& spec, const Material& material, Sheet& sheet, std::string& error);

//! A rigid motion sampled from outside the model (the scene, through the
//! solver contract's ISceneInputs::SampleTransform). At never fails: a source
//! error is latched by the implementation and reported by the solver after
//! the step, which then fails.
class TransformTrack {
public:
	virtual ~TransformTrack() = default;
	//! The rigid map x -> R x + c at time t (R row-major, proper orthonormal).
	virtual void At(double t, double rotation[9], double translation[3]) const = 0;
};

struct Motion {
	enum class Type { Fixed, Translate, Rotate, Oscillate, Track };
	Type type = Type::Fixed;
	V3 vector{{0, 0, 0}};   // Translate: velocity (m/s); Oscillate: amplitude (m)
	V3 center{{0, 0, 0}};   // Rotate: point on the axis
	V3 axis{{0, 1, 0}};     // Rotate: unit axis
	double rate = 0;        // Rotate: rad/s
	double frequency = 0;   // Oscillate: Hz
	//! Track: rest positions (time 0) move by the track's map at t composed
	//! with the inverse of its map at time 0; velocities by a central
	//! difference over 2 microseconds.
	std::shared_ptr<const TransformTrack> track;
	V3 Position(const V3& rest, double t) const;
	V3 Velocity(const V3& rest, double t) const;
	//! The rigid map from time t0 to t1 applied to a point x (where a point
	//! at x at time t0 is at time t1).
	V3 Advance(const V3& x, double t0, double t1) const;
	bool Moving() const { return type != Type::Fixed; }
};
struct PinGroup {
	std::vector<std::uint32_t> vertices;
	Motion motion;
};
struct Collider {
	enum class Type { Plane, Sphere, Box, Cylinder };
	Type type = Type::Plane;
	V3 a{{0, 0, 0}}, b{{0, 1, 0}}; // plane: point, unit normal; sphere: centre; box: min, max;
	                               // cylinder (infinite): point on the axis, unit axis
	double radius = 0;
	//! Signed distance (positive outside), its gradient and, when `hessian` is
	//! given, its Hessian (3x3 row-major; valid outside the shape).
	double Distance(const V3& x, V3& gradient, double* hessian = nullptr) const;
};

//! A triangle-mesh collider: rest vertices moved by a rigid prescribed motion.
struct MeshCollider {
	std::vector<double> vertices;          // 3 per vertex, positions at motion time 0 (m)
	std::vector<std::uint32_t> triangles;  // 3 per triangle
	Motion motion;
};

enum class ContactModel { Penalty, Barrier };
const char* ContactModelName(ContactModel c);

//! Barrier contact (bake quality; ClothContact.h).
struct BarrierSettings {
	double stiffness = 1e5;          // kappa, N/m^3: energy kappa A b(z) per unit cloth area
	double activation = 1e-3;        // d-hat, m: the barrier acts for 0 < z < d-hat
	double friction = 0.3;           // Coulomb coefficient (static = kinetic)
	double frictionVelocity = 1e-3;  // eps_v, m/s: slip speed below which friction is smoothed (sticking)
	bool selfCollision = false;
};

struct ModelConfig {
	Material material;
	V3 gravity{{0, -9.81, 0}};
	std::vector<PinGroup> pins;
	std::vector<Collider> colliders;
	std::vector<MeshCollider> meshColliders; // barrier contact only
	ContactModel contact = ContactModel::Penalty;
	BarrierSettings barrier;
	double contactStiffness = 1e5;  // penalty: N/m^3 (per unit vertex area per unit depth)
	double contactThickness = 0.002; // m: penalty offset; barrier: the cloth's distance offset xi
	double pressure = 0;            // Pa on the front face, pushing along -normal
	double dragCoefficient = 0, airDensity = 1.2;
	bool damping = true;            // apply the material's Rayleigh damping
};

struct NewtonOptions {
	int maxIterations = 50;
	double tolerance = 1e-6;        // converged when max_i |grad_i| / m_i <= tolerance (m/s^2)
	double cgTolerance = 1e-5;      // relative CG residual
	int cgIterations = 4000;
	LinearSolver solver = LinearSolver::Auto;
	bool requireConvergence = true; // false: accept the iterate after maxIterations (interactive)
	bool exactHessianFirst = true;  // try the unprojected Hessian before the projected one
	bool lagHessian = false;        // modified Newton: keep the first iteration's Hessian (and its
	                                // preconditioner) for the whole solve (interactive)
	int maxHalvings = 4;
};

struct StepStats {
	int newtonIterations = 0, cgIterations = 0, directSolves = 0, halvings = 0, unconverged = 0, projections = 0;
	int solves = 0;                 // Newton solves finished
	int ccdLimited = 0;             // line searches whose first trial CCD shortened
	int carried = 0;                // steps that started by carrying vertices with a moving collider
	double residual = 0;            // last residual (m/s^2)
	double residualSum = 0;         // sum of the final residuals of finished solves
	void Add(const StepStats& o) {
		newtonIterations += o.newtonIterations; cgIterations += o.cgIterations; directSolves += o.directSolves;
		halvings += o.halvings; unconverged += o.unconverged; projections += o.projections; residual = o.residual;
		solves += o.solves; residualSum += o.residualSum; ccdLimited += o.ccdLimited; carried += o.carried;
	}
};

class ClothSystem {
public:
	bool Initialize(const Sheet& sheet, const ModelConfig& config, std::string& error);
	const Sheet& GetSheet() const { return sheet_; }
	const ModelConfig& Config() const { return config_; }
	std::uint32_t Vertices() const { return sheet_.Vertices(); }
	bool Fixed(std::uint32_t v) const { return fixed_[v] != 0; }

	//! Threads for assembly and the linear solver (1: the caller only).  Results
	//! are bitwise the same for any count (ClothParallel.h).
	void SetThreads(unsigned threads);
	unsigned Threads() const { return pool_ ? pool_->Threads() : 1; }

	//! x = initial placement (pins at time t), v = pin velocities, history cleared.
	void Reset(double t);
	//! One step of length h from `time` (pins, loads and collider state at the
	//! step's times).  Returns false only when the step and its halvings fail.
	bool Step(double h, Scheme scheme, const NewtonOptions& options, StepStats& stats, std::string& error);
	//! Static equilibrium from the current x by damped pseudo-time continuation
	//! (stages with a mass term |x - x_k|^2_M / (2 tau^2), tau growing by 4) then
	//! full Newton (unless `finish` is false: a problem with free rigid modes,
	//! such as a sheet on a frictionless plane, keeps the last stage's mass
	//! term).  Leaves v = 0.
	bool SolveStatic(const NewtonOptions& options, StepStats& stats, std::string& error,
		double tau0 = 0.01, int stages = 8, bool finish = true);

	// State (public for the solver's checkpoint and for validation).
	std::vector<double> x, v, xPrev, vPrev;
	double hPrev = 0, time = 0;
	bool havePrev = false;
	std::vector<double> portPressure;  // per vertex, Pa (empty: none)
	std::vector<double> portTraction;  // 3 per vertex, Pa (empty: none)

	// Barrier contact (null without it) and diagnostics for validation.
	const BarrierContact* Barrier() const { return barrier_.get(); }
	//! Smallest gap d - xi over the barrier's pairs at x with colliders at time t (infinity without contact).
	double ContactGap(const std::vector<double>& x, double t) const;
	//! Total barrier force on the cloth at x (N), colliders at time t.
	V3 ContactForce(const std::vector<double>& x, double t) const;
	//! Barrier energy at x (no friction), colliders at time t.
	double ContactEnergy(const std::vector<double>& x, double t) const;
	//! Exact check of the straight path x0 -> x1 against the colliders at time t.
	bool PathIntersectionFree(const std::vector<double>& x0, const std::vector<double>& x1, double t, std::string& detail) const;

	// Diagnostics.
	double ElasticEnergy(const std::vector<double>& positions) const; // membrane + bending
	void ElasticGradient(const std::vector<double>& positions, std::vector<double>& gradient) const;
	double KineticEnergy(const std::vector<double>& velocities) const;
	double GravityPotential(const std::vector<double>& positions) const;
	void VertexNormals(const std::vector<double>& positions, std::vector<double>& normals) const;
	//! Projected elastic Hessian (membrane + bending) at `positions`, pinned rows/cols
	//! replaced by identity.  Equals the linearised operator at a flat unstressed state.
	void ElasticHessian(const std::vector<double>& positions, BlockMatrix& h) const;
	//! Solves A x = b with the configured solver policy (for validation).
	//! `reuse`: `a` is the matrix of the previous call, unchanged (keeps its factor or preconditioner).
	bool Solve(const BlockMatrix& a, const std::vector<double>& b, std::vector<double>& solution,
		const NewtonOptions& options, bool preferDirect, StepStats& stats, bool* indefinite = nullptr,
		bool reuse = false) const;

private:
	struct Potential {
		double inertia = 0;                 // 1/hhat^2 (0: none)
		std::vector<double> xHat;
		double dampingScale = 0;            // c (0: no damping term)
		std::vector<double> xTilde;
		std::vector<double> force, xRef;    // constant external force and its reference point
		bool useDamping = false;
		std::shared_ptr<ContactFrame> contact; // barrier contact state (null: none)
	};
	double Evaluate(const Potential& p, const std::vector<double>& pos, std::vector<double>* gradient,
		BlockMatrix* hessian, bool project) const;
	double ElementEnergies(const std::vector<double>& pos, std::vector<double>* gradient, BlockMatrix* hessian,
		bool contact, double allowance) const;
	bool Newton(const Potential& p, std::vector<double>& pos, const NewtonOptions& options, bool preferDirect,
		StepStats& stats, std::string& error) const;
	bool StepOnce(double h, Scheme scheme, const NewtonOptions& options, StepStats& stats, std::string& error);
	bool StepRecursive(double h, Scheme scheme, const NewtonOptions& options, int depth, StepStats& stats,
		std::string& error);
	void ExternalForce(const std::vector<double>& pos, const std::vector<double>& vel, std::vector<double>& f) const;
	void PinTargets(double t, std::vector<double>& pos, std::vector<double>* vel) const;
	void MaskFixed(BlockMatrix& h) const;

	Sheet sheet_;
	ModelConfig config_;
	double c11_ = 0, c12_ = 0, c22_ = 0, c66_ = 0;
	std::vector<char> fixed_;
	std::vector<int> pinGroup_;               // per vertex: group index or -1
	BlockMatrix pattern_, damping_;
	std::vector<std::array<std::size_t,9>> triSlots_;
	std::vector<std::array<std::size_t,16>> hingeSlots_;
	std::vector<std::array<std::size_t,9>> rodSlots_;
	std::vector<double> hingeScale_, rodScale_; // clamp correction (1 or 2)
	std::vector<std::uint32_t> order_;        // banded ordering
	double directCost_ = 0;                   // dofs * bandwidth^2 under order_
	mutable GridMultigrid multigrid_;         // CG preconditioner on the sheet's grid
	std::shared_ptr<ParallelPool> pool_;
	std::shared_ptr<BarrierContact> barrier_;
	// Elements by colour (greedy, in element order): the elements of one colour
	// share no vertex, so they assemble in parallel without conflicts.
	std::vector<std::vector<std::uint32_t>> triColors_, hingeColors_, rodColors_;
	mutable std::vector<double> elementEnergy_; // per element: triangles, hinges, rods
	mutable BandedCholesky factor_;           // the last direct factorisation
	mutable bool factorValid_ = false;
};

} } }

#endif
