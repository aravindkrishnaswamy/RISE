//////////////////////////////////////////////////////////////////////
//
//  LiquidBodies.h - Rigid bodies inside the liquid solver (simulation
//  M7b, docs/SIMULATION_SOLVER_CONTRACT.md): moving cut-cell geometry rebuilt
//  from each body's pose, the solid face velocities from its twist, and
//  the coupled pressure solve of a dynamic body (Batty, Bertails and
//  Bridson 2007, section 4).
//
//  Geometry.  A body is a sphere, a box or a capped cylinder (axis z) in
//  its body frame, whose origin is the centre of mass (the frame of
//  rigid.bodies for these shapes).  Every step the node signed distance is
//  the minimum of the static solids' (computed once) and each body's exact
//  distance at its new pose, inside a box around the body; the face open
//  fractions and the cell open fractions are recomputed in those boxes
//  only, with the same per-face formula as the static solids (LiquidGrid.h).
//  The closed part of a face belongs to the solid whose distance at the
//  face centre is the smallest (a body or the static solids and walls).
//
//  Coupled pressure (derived here from the variational principle; the
//  derivation is in docs/SIMULATION_SOLVER_CONTRACT.md section 2).  A face f of
//  axis a closed by a body carries the body velocity at its centre,
//  u_s,f = g_f . w, g_f = (e_a, (x_f - X) x e_a), w = (V, Omega).  The
//  projection minimises the kinetic energy
//      (rho h^3 / 2) sum_f A_f theta_f (u_f - u*_f)^2 + (1/2) (w - w*)^T M (w - w*)
//  subject to the discrete divergence of every liquid cell,
//      sum_f s_cf [A_f u_f + (1 - A_f) u_s,f] = 0.
//  With the multiplier q (the solver's scaled pressure q = p dt / (rho h)),
//  u_f = u*_f + sum_c s_cf q_c and w = w* + rho h^3 M^-1 J q with
//      J_c = sum_{f of c, closed by the body} s_cf (1 - A_f) g_f   (6 x 1),
//  and the system becomes (A + rho h^3 J^T M^-1 J) q = b: the grid operator
//  plus a symmetric positive semidefinite term of rank at most six per body.
//  b already holds sum_c J_c^T w* through the solid face velocities.  The
//  hydrodynamic impulse on the body is rho h^3 J q (force dt, torque dt
//  about the centre of mass); for a constant pressure p_c in a cell it is
//  the exact integral of -p n over the solid surface inside that cell.
//  M = diag(m 1, R I R^T) at the body's new pose; locked degrees of freedom
//  have infinite mass.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_LIQUID_BODIES_H
#define RISE_SIMULATION_LIQUID_BODIES_H

#include "LiquidFlow.h"
#include "../Rigid/RigidBody.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Liquid {

using Quat = Rigid::Quat;
using Vec6 = std::array<double,6>;

//! m7d: a closed triangle mesh as a body (MakeMeshShape).  Its signed
//! distance in the body frame (the centroid, principal axes) is computed once
//! on a lattice of spacing `spacing` covering the mesh and `pad` beyond it:
//! at each lattice node the exact distance to the nearest triangle (a
//! bounding-volume tree), the sign by the parity of crossings along +x (a
//! closed mesh).  Distance() interpolates trilinearly (exact for a plane;
//! O(spacing^2 curvature) otherwise) and returns pad plus the distance to the
//! lattice beyond it, a lower bound of the true distance there.
struct MeshDistance {
	V3 origin{{0, 0, 0}};
	double spacing = 1, pad = 0;
	std::array<int,3> nodes{{0, 0, 0}};
	std::vector<double> d;                     // per node, x fastest
	std::vector<double> vertices;              // body frame, 3 per vertex
	std::vector<std::uint32_t> triangles;
	double volume = 0, boundingRadius = 0, smallestExtent = 0;
	V3 unitMoments{{0, 0, 0}};                 // principal moments at unit density (kg m^2 per kg/m^3)
	double Sample(const V3& y) const;
};

//! A body's shape in its body frame (centred at the centre of mass).
struct BodyShape {
	enum class Type { Sphere, Box, Cylinder, Mesh };
	Type type = Type::Sphere;
	double radius = 0.1, height = 0.2;   // sphere, cylinder (axis z, capped, total length)
	V3 size{{0.2, 0.2, 0.2}};            // box: full extents
	std::shared_ptr<const MeshDistance> mesh;   // Mesh (m7d)
	//! Exact signed distance at a body-frame point (negative inside).
	double Distance(const V3& local) const;
	double BoundingRadius() const;
	double Volume() const;
	//! Principal moments of inertia about the centre for a total mass.
	V3 Moments(double mass) const;
	//! The smallest extent (a diameter, or the smallest box side).
	double SmallestExtent() const;
};
const char* BodyShapeName(BodyShape::Type t);

//! m7d: a mesh body from a closed, outward-oriented triangle mesh in its
//! authoring frame.  The volume, centroid and inertia come from the mesh by
//! the divergence theorem (Rigid::ShapeMass, which refuses an open or
//! inconsistently oriented mesh); the body frame is the centroid and the
//! principal axes (Rigid::PrincipalAxes: the frame rigid.bodies gives the same
//! mesh), so a body at authoring pose (position p, rotation Ra) has its centre
//! of mass at p + Ra centroid and orientation Ra axes.  `spacing` is the
//! distance lattice's (the liquid uses h / 2), `pad` its margin beyond the
//! mesh (at least the liquid's band plus a cell).
bool MakeMeshShape(const std::vector<double>& vertices, const std::vector<std::uint32_t>& triangles, double spacing, double pad,
	BodyShape& shape, V3& centroid, Rigid::M3& axes, std::string& error);

enum class BodyMotion {
	Dynamic,      // integrated by the liquid (mass from its density or mass), coupled in the pressure solve
	Prescribed,   // pose and twist given as functions of time
	Port          // pose and twist from a rigid.bodies port; coupled (implicit) or kinematic (staggered)
};

struct LiquidBody {
	std::string name;
	BodyShape shape;
	BodyMotion motion = BodyMotion::Dynamic;
	bool staggered = false;               // Port: kinematic in the solve, the measured force offered afterwards
	std::array<bool,4> free{{true, true, true, true}};   // translation x, y, z; rotation
	//! State: x centre of mass (world), q body -> world, v, pi (body-frame
	//! angular momentum), mass, moments.  Rigid::Body's conventions.
	Rigid::Body state;
	V3 externalForce{{0, 0, 0}};          // constant load besides the liquid (Dynamic: m g; Port: from the port)
	// Prescribed motion: x(t) = x0 + v0 t + amplitude sin(2 pi f t + phase), q(t) = exp(t spin) q0.
	V3 x0{{0, 0, 0}}, v0{{0, 0, 0}}, amplitude{{0, 0, 0}}, spin{{0, 0, 0}};
	Quat q0{{1, 0, 0, 0}};
	double frequency = 0, phase = 0;

	bool Coupled() const { return motion == BodyMotion::Dynamic || (motion == BodyMotion::Port && !staggered); }
	//! Prescribed pose and twist at time t.
	void PrescribedAt(double t, V3& x, Quat& q, V3& v, V3& omega) const;
	V3 Omega() const { return state.AngularVelocity(); }
	//! Speed bound of the body's surface (|v| + |omega| R).
	double SurfaceSpeed() const;
};

//! A face closed (partly) by a body.
struct BodyFace {
	int axis = 0;
	std::size_t face = 0;
	double closed = 0;       // 1 - A_f
	Vec6 g{{0, 0, 0, 0, 0, 0}};   // (e_a, (x_f - X) x e_a)
};

//! The moving solid geometry: the static solids' node distances and
//! fractions (computed once), and per step the union with the bodies.
class BodyGeometry {
public:
	//! The static part: node distances, face and cell fractions as LiquidModel
	//! computes them without bodies.
	void Initialize(const Grid& g, const std::vector<double>& staticNodes, const std::array<std::vector<double>,3>& staticFractions,
		const std::vector<double>& staticOpen, bool openTop, double band);
	//! Rebuilds nodes, fractions, open cells and the body faces for the bodies
	//! at their current poses (the region each body covered at the last call
	//! and covers now is recomputed; elsewhere the static values stand).
	void Update(const std::vector<LiquidBody>& bodies, std::vector<double>& nodes, std::array<std::vector<double>,3>& fraction,
		std::vector<double>& open, Pool* pool);
	//! Per body: the faces it closes (sorted by axis and index).
	const std::vector<std::vector<BodyFace>>& Faces() const { return faces_; }
	//! The solid face-velocity field: zero except on body faces, where it is
	//! g_f . w for each body's w (index matches `bodies`).
	void SolidVelocity(const std::vector<Vec6>& w, MacField& solid) const;
	//! Cells whose faces are all closed and that lie inside a body.
	const std::vector<std::size_t>& EnclosedCells() const { return enclosed_; }
private:
	struct Box { int lo[3], hi[3]; bool empty = true; };
	Box NodeBox(const LiquidBody& b) const;
	Grid g_;
	bool openTop_ = false;
	double band_ = 0;
	std::vector<double> staticNodes_, staticOpen_;
	std::array<std::vector<double>,3> staticFractions_;
	std::vector<Box> last_;
	std::vector<std::vector<BodyFace>> faces_;
	std::vector<std::size_t> enclosed_;
	mutable std::array<std::vector<std::size_t>,3> touched_;   // faces whose solid velocity was set last time
};

//! The coupling of one body with free degrees of freedom: J (sparse, per
//! liquid cell), M^-1 and the impulse of the last solve.
class BodyCoupling final : public IPressureCoupling {
public:
	//! `faces` as BodyGeometry gives them; `density` the liquid's; the body's
	//! current pose (M = diag(m 1, R I R^T)).
	BodyCoupling(const Grid& g, const std::vector<BodyFace>& faces, const LiquidBody& body, double density, bool coupled);
	void Begin(const std::vector<std::uint8_t>& active) override;
	void ApplyAdd(const std::vector<double>& q, std::vector<double>& out) const override;
	void AddRhs(std::vector<double>&) const override {}
	void Finish(const std::vector<double>& q) override;
	//! rho h^3 J q of the last Finish (N s; N m s about the centre of mass).
	const Vec6& Impulse() const { return impulse_; }
	//! rho h^3 J q for any q (the measured impulse on a kinematic body).
	Vec6 Measure(const std::vector<double>& q) const;
	//! out = M^-1 v (locked degrees of freedom: 0).
	Vec6 InverseMass(const Vec6& v) const;
	//! The cells and their J entries (sorted by cell): for tests.
	const std::vector<std::pair<std::size_t, Vec6>>& Entries() const { return entries_; }
	bool Coupled() const { return coupled_; }
private:
	const Grid& g_;
	const std::vector<BodyFace>& faces_;
	double scale_ = 0;          // rho h^3
	double invMass_ = 0;
	std::array<bool,4> free_;
	std::array<double,9> invInertia_{};   // world, row-major
	bool coupled_ = true;
	std::vector<std::pair<std::size_t, Vec6>> entries_;
	Vec6 impulse_{{0, 0, 0, 0, 0, 0}};
};

//! The world-frame inertia tensor R diag(moments) R^T and its inverse.
void WorldInertia(const Rigid::Body& b, std::array<double,9>& inertia, std::array<double,9>& inverse);

} } }

#endif
