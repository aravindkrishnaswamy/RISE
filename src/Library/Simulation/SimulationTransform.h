// Versioned native payload for a set of named rigid bodies at one time
// ("transform.v1", simulation M6): position, orientation (unit quaternion),
// linear and angular velocity per body, in stated length units.  Used as the
// cache output of a rigid-body solver and as its coupling port payload
// (body_state).  See docs/SIMULATION_TRANSFORM_FORMAT.md.
#ifndef RISE_SIMULATION_TRANSFORM_H
#define RISE_SIMULATION_TRANSFORM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! Codec name of the payload (cache output type, port codec).
constexpr const char* TransformCodec = "transform.v1";

//! One rigid body at one time.  The body frame maps body coordinates b to
//! world coordinates x = R(orientation) b + position; position is the body
//! frame's origin (for a rigid-body solver: the centre of mass) in the
//! frame's length units.  orientation is (w, x, y, z), unit, canonical sign
//! (w > 0, or w = 0 and the first nonzero of x, y, z positive).  velocity is
//! the origin's velocity (length units / s), angularVelocity the world-frame
//! angular velocity (rad/s).
struct TransformBody {
	std::string name;
	std::array<double,3> position{{0, 0, 0}};
	std::array<double,4> orientation{{1, 0, 0, 0}};
	std::array<double,3> velocity{{0, 0, 0}};
	std::array<double,3> angularVelocity{{0, 0, 0}};
};

//! A set of bodies at one time.  Bodies are sorted by unique name (unsigned
//! UTF-8 bytes); names and units are nonempty NFC text without NUL; every
//! number is finite binary64.
struct TransformFrame {
	std::string lengthUnits = "m";
	double time = 0;                    // seconds, the frame's own time stamp
	std::vector<TransformBody> bodies;
};

// The budget covers retained input, the previous output, the candidate and
// validation scratch.  Failure leaves output untouched.  Encoding is
// canonical: every valid frame has exactly one encoding; a frame whose
// quaternion is not unit (|1 - |q|| > 1e-9) or not in the canonical sign is
// refused, never silently changed.
bool EncodeTransforms(const TransformFrame& frame, std::size_t budget, std::vector<std::uint8_t>& out,
	std::string& error);
bool DecodeTransforms(const std::vector<std::uint8_t>& bytes, std::size_t budget, TransformFrame& out,
	std::string& error);

//! The quaternion scaled to unit length with the canonical sign (false for a
//! zero or nonfinite quaternion).
bool CanonicalQuaternion(std::array<double,4>& q);
//! Rotation matrix (row-major) of a unit quaternion (w, x, y, z).
std::array<double,9> QuaternionMatrix(const std::array<double,4>& q);
//! Unit quaternion (canonical sign) of a proper rotation matrix (row-major).
std::array<double,4> MatrixQuaternion(const std::array<double,9>& r);

//! The field.v1 domain digest of per-body data over a set of body names
//! (sorted, unique): SHA-256 of canonical CBOR
//! ["rise.simulation.bodies.v1", [name...]].
std::string TransformBodyDomain(const std::vector<std::string>& sortedNames);

} }
#endif
