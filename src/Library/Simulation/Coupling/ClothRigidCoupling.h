// Simulation M6: the cloth <-> rigid-body coupling (cloth.shell with a
// port-driven mesh collider, rigid.bodies) -- the canonical problems of its
// evidence and of the M6 demonstrations, the group they run in, and the
// momentum / energy totals they are judged by (docs/SIMULATION_M6_RESULTS.md).
//
// The coupling: rigid body_state -> rigid_surface -> cloth collider_surface
// (the collider follows the body), cloth collider_reaction -> contact_reaction
// -> rigid body_load (the body feels the barrier and friction forces the
// cloth felt, as the impulse over the step about a fixed point, so linear and
// angular momentum cross the port exactly).
//
// Its coupling evidence (kind "contact_reaction+rigid_surface",
// RegisterClothRigidCouplingEvidence) runs the zero-gravity problem with
// implicit Gauss-Seidel coupling and states, digest-bound: the drift of the
// total linear momentum, the drift of the total angular momentum and the
// largest rise of the kinetic energy, each against a tolerance.
#ifndef RISE_SIMULATION_CLOTH_RIGID_COUPLING_H
#define RISE_SIMULATION_CLOTH_RIGID_COUPLING_H

#include "../SimulationGroup.h"
#include <array>

namespace RISE { namespace Simulation {

constexpr const char* ClothRigidCouplingKind = "contact_reaction+rigid_surface";

struct ClothRigidScenario {
	RISECBOR64::Value rigid, cloth;      // member parameters
	std::string body;                    // the coupled body
	double mass = 0;                     // its mass and principal moments
	std::array<double,3> moments{{0, 0, 0}};
	std::vector<double> clothMasses;     // lumped mass per cloth vertex
	double duration = 0;
};

//! Zero gravity, no pins: a 0.3 m box (0.81 kg) moving up at 0.5 m/s and
//! spinning at 2 rad/s about the vertical into a free 0.6 m cotton sheet
//! (`vertices` per side) 1 cm above it; backward Euler cloth, no damping,
//! friction 0.3.  Output rate 240 / substeps Hz, `frames` frames.
bool ZeroGravitySpinScenario(std::uint64_t substeps, std::uint64_t frames, ClothRigidScenario& out, std::string& error,
	std::uint32_t vertices = 13);
//! A sheet falling under gravity onto a floating box of `mass` kg (the box
//! has no gravity: only the sheet loads it); BDF2 cloth.
bool FloatingBoxScenario(double mass, std::uint64_t frames, ClothRigidScenario& out, std::string& error);
//! The trampoline: a 0.1 m ball of `mass` kg dropped from 0.25 m onto a 0.8 m
//! sheet (15 x 15) pinned at its four corners, gravity on both; BDF2 cloth.
bool TrampolineScenario(double mass, std::uint64_t frames, ClothRigidScenario& out, std::string& error);

//! Members "body" (rigid.bodies) and "sheet" (cloth.shell), the two
//! connections, then Configure (no scene inputs).
bool BuildClothRigidGroup(SimulationGroup& group, const ClothRigidScenario& scenario, const GroupPolicy& policy,
	std::string& error);
//! The same sheet fed one-way from a cached rigid run ("body" a cached member).
bool BuildClothFromCachedRigid(SimulationGroup& group, const ClothRigidScenario& scenario,
	std::shared_ptr<const IReadOnlyCacheSnapshot> rigid, const GroupPolicy& policy, std::string& error);

struct ClothRigidTotals {
	std::array<double,3> momentum{{0, 0, 0}}, angular{{0, 0, 0}};  // linear; angular about the origin
	double kinetic = 0;
	std::array<double,3> bodyPosition{{0, 0, 0}};
};
//! Totals of sheet + body at an output index, from the members' published frames.
bool MeasureClothRigid(const IReadOnlyCacheSnapshot& sheet, const IReadOnlyCacheSnapshot& body, std::int64_t index,
	const ClothRigidScenario& scenario, ClothRigidTotals& totals, std::string& error);

//! Registers the coupling evidence of ClothRigidCouplingKind (computed on first use).  Idempotent.
bool RegisterClothRigidCouplingEvidence(std::string& error);

} }
#endif
