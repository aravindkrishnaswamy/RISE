// Simulation M6: port adapters (the contract's IPortAdapter, SimulationHost.h)
// and their factory registry.  An adapter converts what one solver offers into
// what another accepts; adapters are pure functions of their input bytes
// (deterministic), state the label their output may carry and what they
// cover, and their identity (name, version, parameters) enters the cache key
// of the member fed through them (SimulationGroup.h).
//
// Built-in adapters (RegisterBuiltinPortAdapters):
//   unit_scale        field.v1 / transform.v1 / grid.v1 -> the same codec with other
//                     units: one scalar factor per channel from a small explicit table
//                     (UnitFactor), no unit algebra.  Exact up to binary32 rounding.
//   field_relabel     field.v1 -> field.v1 on the identical domain: channel names
//                     (and quantities) relabelled, values copied.  Exact.
//   rigid_surface     transform.v1 (a body) + its surface mesh (body frame) ->
//                     mesh.v2 world positions and velocities (v + w x r), inline
//                     topology: a moving collider surface.  Exact placement.
//   aerodynamic_drag  grid.v1 gas velocity (and density, or temperature by the ideal
//                     gas law) + a surface_state mesh (auxiliary) -> field.v1 per-vertex
//                     traction: quadratic drag on the relative velocity, normal and
//                     tangential coefficients stated.  A MODEL: preview.
//   contact_reaction  field.v1 per-collider reaction (force, point, torque) ->
//                     field.v1 body_load over a body set.  Exact transfer.
#ifndef RISE_SIMULATION_PORT_ADAPTERS_H
#define RISE_SIMULATION_PORT_ADAPTERS_H

#include "SimulationHost.h"
#include <functional>

namespace RISE { namespace Simulation {

constexpr const char* UnitScaleAdapterId = "unit_scale";
constexpr const char* FieldRelabelAdapterId = "field_relabel";
constexpr const char* RigidSurfaceAdapterId = "rigid_surface";
constexpr const char* AerodynamicDragAdapterId = "aerodynamic_drag";
constexpr const char* ContactReactionAdapterId = "contact_reaction";

//! What a factory may read besides its parameters: the two port
//! declarations and the cache descriptions of the offering and accepting
//! members (their physical records, e.g. a rigid body's surface).
struct PortAdapterContext {
	const PortDeclaration* offer = nullptr;
	const PortDeclaration* accept = nullptr;
	const CacheDescription* offering = nullptr;
	const CacheDescription* accepting = nullptr;
};
using PortAdapterFactory = std::function<std::shared_ptr<const IPortAdapter>(const RISECBOR64::Value& parameters,
	const PortAdapterContext& context, std::string& error)>;

//! Process-wide, thread-safe; a duplicate id is refused.
bool RegisterPortAdapterFactory(const std::string& id, PortAdapterFactory factory, std::string& error);
//! An adapter instance for a connection (null with an error when the id is
//! unknown, the parameters are refused or the declarations do not fit).
std::shared_ptr<const IPortAdapter> CreatePortAdapter(const std::string& id, const RISECBOR64::Value& parameters,
	const PortAdapterContext& context, std::string& error);
std::vector<std::string> RegisteredPortAdapterFactories();
//! The built-in factories, and the parameterless ones (unit_scale,
//! field_relabel) also with RegisterPortAdapter so FindPortAdapter finds
//! them.  Idempotent.
bool RegisterBuiltinPortAdapters(std::string& error);

//! SHA-256 of canonical CBOR ["rise.simulation.adapter.v1", id, version, parameters].
std::string PortAdapterIdentity(const std::string& id, const std::string& version, const RISECBOR64::Value& parameters);

//! The scalar factor from one unit to another of the same quantity, from a
//! small explicit table (length m cm mm km in ft; velocity m/s cm/s mm/s km/h;
//! mass kg g; density kg/m^3 g/m^3 g/cm^3; force N kN mN; torque N m N mm;
//! pressure Pa kPa MPa bar; temperature K only; dimensionless 1).  Identical
//! units give 1.  False when the pair is not in the table.
bool UnitFactor(const std::string& from, const std::string& to, double& factor);

} }
#endif
