//////////////////////////////////////////////////////////////////////
//
//  RigidSolver.h - "rigid.bodies" on the solver contract (simulation M6)
//
//  A small, physically honest rigid-body solver: free bodies with mass and
//  principal inertia from density and shape (box, sphere, cylinder, closed
//  mesh, or a scene object's geometry), gravity, held external loads from a
//  port, and impulse contact with static planes (preview).  No joints, no
//  body-body contact.  Integrator: kick-drift-kick with the symplectic
//  principal-axis splitting for the free rotation (RigidBody.h).
//
//  Output "bodies" (transform.v1, SimulationTransform.h).  Ports: offers
//  "body_state" (the same payloads), accepts "body_load" (field.v1 over the
//  body set, TransformBodyDomain of the sorted names; channels "force" N x3,
//  "point" m x3 (world point of application; absent: the centre of mass),
//  "torque" N m x3), read at the start of each macro step and held.
//  Honours imposed macro steps; rollback; checkpoints; deterministic.
//
//  Evidence (computed at Configure for the configured substep length,
//  memoised per process, digest-bound): torque-free symmetric top against
//  its closed-form solution; an asymmetric body spun near its intermediate
//  axis (Dzhanibekov) keeping energy and angular momentum over many flips;
//  projectile motion; a physical pendulum held by a stiff external spring
//  against the exact period.  "predictive" is requested only without plane
//  contact.  Parameters: docs/SIMULATION_SOLVER_CONTRACT.md.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_RIGID_SOLVER_H
#define RISE_SIMULATION_RIGID_SOLVER_H

#include "../SimulationSolver.h"
#include <string>

namespace RISE { namespace Simulation {

//! Solver id of the rigid-body solver.
constexpr const char* RigidSolverId = "rigid.bodies";
//! Format of the record the solver embeds in its cache's physicalRecords:
//! per body its name, mass, principal moments and its surface in the body
//! frame (the adapter rigid_surface reads it).
constexpr const char* RigidBodiesRecordFormat = "rigid.bodies.v1";

//! Registers the rigid-body solver factory under "rigid.bodies".  Idempotent.
bool RegisterRigidSolver(std::string& error);

//! One body's surface in its body frame, read back from a configured
//! description's physicalRecords (false when the record or body is absent).
bool RigidBodySurface(const Bytes& physicalRecords, const std::string& body, std::vector<double>& vertices,
	std::vector<std::uint32_t>& triangles, std::string& error);

} }

#endif
