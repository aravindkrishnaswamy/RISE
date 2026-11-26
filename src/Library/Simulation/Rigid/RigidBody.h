//////////////////////////////////////////////////////////////////////
//
//  RigidBody.h - The rigid-body model of simulation M6: mass properties,
//  a symplectic splitting integrator and an impulse plane contact
//
//  Mass properties from density and shape: box, sphere, cylinder (axis
//  along body z), and a closed, outward-oriented triangle mesh by its
//  polyhedral mass properties (divergence theorem over the signed
//  tetrahedra from the origin: volume, first and second moments exactly;
//  Mirtich 1996 / Eberly's formulation).  The body frame is the principal
//  frame with its origin at the centre of mass; the analytic shapes keep
//  their authoring axes (their inertia is already diagonal there).
//
//  Integration (one step of length h, external force F applied at a world
//  point p plus a torque tau, both held over the step, and gravity g):
//    kick    v += h/2 (F/m + g);  Pi += h/2 R^T (tau + (p - x) x F)
//    drift   x += h v;  free rotation of (R, Pi) for h
//    kick    v += h/2 (F/m + g);  Pi += h/2 R^T (tau + (p - x) x F)
//  The free rotation is the symplectic splitting of Dullweber, Leimkuhler
//  and McLachlan (1997): H = sum_i Pi_i^2 / (2 I_i) split into its three
//  principal terms, each an exact rotation about one body axis, composed
//  symmetrically (1/2, 2/2, 3, 2/2, 1/2).  Every sub-flow preserves the
//  spatial angular momentum R Pi and |Pi| exactly (up to round-off); the
//  energy error is bounded, O(h^2), with no drift.  The kicks change the
//  angular momentum about the world origin by exactly h (tau + p x F), so
//  an impulse exchanged through a coupling port is conserved exactly.
//  Second order overall.
//
//  Plane contact (preview, documented, not validated): after each step,
//  support points (sphere: the lowest point; box: corners; cylinder: rim
//  points; mesh: vertices) below a plane get sequential impulses with
//  Newton restitution e (0 below a resting speed) and Coulomb friction mu
//  (sticking up to the cone), then the body is moved out along the normal.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_RIGID_BODY_H
#define RISE_SIMULATION_RIGID_BODY_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation { namespace Rigid {

using V3 = std::array<double,3>;
using M3 = std::array<double,9>;   // row-major
using Quat = std::array<double,4>; // (w, x, y, z)

V3 Add(const V3& a, const V3& b);
V3 Sub(const V3& a, const V3& b);
V3 Mul(const V3& a, double s);
V3 Cross(const V3& a, const V3& b);
double Dot(const V3& a, const V3& b);
double Norm(const V3& a);
V3 Apply(const M3& m, const V3& a);          // m a
V3 ApplyT(const M3& m, const V3& a);         // m^T a
M3 Multiply(const M3& a, const M3& b);
Quat QMul(const Quat& a, const Quat& b);
Quat QAxis(int axis, double angle);          // rotation by angle about body axis 0, 1 or 2
M3 QMatrix(const Quat& q);
void QNormalize(Quat& q);

//! Mass properties about the centroid, in the input frame.
struct MassProperties {
	double volume = 0, mass = 0;
	V3 centroid{{0, 0, 0}};
	M3 inertia{{0, 0, 0, 0, 0, 0, 0, 0, 0}};   // kg m^2 about the centroid
};

struct Shape {
	enum class Type { Box, Sphere, Cylinder, Mesh };
	Type type = Type::Box;
	V3 size{{1, 1, 1}};                  // box: full extents
	double radius = 0.5, height = 1;     // sphere, cylinder (axis z)
	std::vector<double> vertices;        // mesh: 3 per vertex
	std::vector<std::uint32_t> triangles;// mesh: 3 per triangle, outward (counter-clockwise from outside)
};
const char* ShapeName(Shape::Type t);

//! Mass properties of a shape at unit density centred as authored (box,
//! sphere, cylinder about the origin; a mesh where its vertices are).  False
//! (with an error) for a degenerate shape or an open / inward mesh.
bool ShapeMass(const Shape& shape, double density, MassProperties& out, std::string& error);
//! Symmetric 3x3 eigen decomposition (Jacobi): inertia = P diag(moments) P^T
//! with P a proper rotation (columns: principal axes), moments ascending.
void PrincipalAxes(const M3& inertia, V3& moments, M3& axes);
//! The surface of a shape as a triangle mesh in the shape's authoring frame
//! (box 12 triangles; sphere an icosphere of 80 triangles; cylinder 32
//! segments; mesh as given).  Outward, counter-clockwise.
void ShapeSurface(const Shape& shape, std::vector<double>& vertices, std::vector<std::uint32_t>& triangles);
//! Support points for plane contact, in the shape's authoring frame.
void ShapeSupportPoints(const Shape& shape, std::vector<double>& points);

//! One body's state and constants.  x: centre of mass (world); q: body ->
//! world; v: velocity of the centre of mass; pi: body-frame angular momentum.
struct Body {
	std::string name;
	double mass = 1;
	V3 moments{{1, 1, 1}};               // principal moments of inertia, kg m^2
	bool sphere = false;                 // the lowest point is the contact point
	double radius = 0;                   // sphere radius
	std::vector<double> support;         // support points, body frame
	V3 x{{0, 0, 0}}, v{{0, 0, 0}}, pi{{0, 0, 0}};
	Quat q{{1, 0, 0, 0}};
	V3 AngularVelocity() const;          // world frame
	double KineticEnergy() const;
	V3 AngularMomentum() const;          // about the world origin: x x m v + R pi
};

//! A load held over a step: force at a world point plus a torque (world).
struct Load {
	V3 force{{0, 0, 0}}, point{{0, 0, 0}}, torque{{0, 0, 0}};
	bool atCentre = true;                // no point: the force acts at the centre of mass
};

struct Plane {
	V3 point{{0, 0, 0}}, normal{{0, 1, 0}};
	double restitution = 0.5, friction = 0.5;
};

//! Torque-free rotation of (q, pi) by the symmetric principal splitting.
void FreeRotation(Body& b, double h);
//! The two halves of StepBody, for a load that depends on the state (the
//! evidence's pendulum): a kick of length h, and a drift (translation and
//! free rotation) of length h.
void KickBody(Body& b, const Load& load, const V3& gravity, double h);
void DriftBody(Body& b, double h);
//! One kick-drift-kick step (see the header comment).
void StepBody(Body& b, const Load& load, const V3& gravity, double h);
//! Plane contact after a step (preview).  Returns the number of impulses applied.
int ResolvePlanes(Body& b, const std::vector<Plane>& planes, const V3& gravity, double h);

} } }

#endif
