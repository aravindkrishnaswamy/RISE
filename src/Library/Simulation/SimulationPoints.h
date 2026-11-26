// Versioned native payload for one particle set at one time ("points.v1",
// simulation M7 prerequisites): positions, optional velocities, an optional
// radius (uniform or per point), optional stable ids and named per-point
// float attributes with units.  Used for liquid marker and spray particles
// and imported particle data.  See docs/SIMULATION_POINTS_FORMAT.md.
#ifndef RISE_SIMULATION_POINTS_H
#define RISE_SIMULATION_POINTS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! Codec name of the payload (cache output type).
constexpr const char* PointsCodec = "points.v1";

//! A named per-point attribute: `components` binary32 samples per point
//! (1 to 16), with units ("1" when dimensionless).  The names `position`,
//! `velocity`, `radius` and `id` are reserved.
struct PointAttribute {
	std::string name, units;
	std::uint8_t components = 1;
	std::vector<std::uint32_t> values;
};

// One particle set at one time.  Samples are opaque IEEE binary32 bit
// strings (finite; bit patterns preserved), xyz interleaved per point, as in
// mesh.v1.  Radii are in the length units and nonnegative (sign bit clear).
// A frame carries a per-point radius, a uniform radius, or neither (the
// consumer then chooses one).  Ids, when present, are strictly increasing
// and give correspondence across frames.  N = 0 is valid.
struct PointsFrame {
	std::string lengthUnits = "m";
	std::string velocityUnits;              // nonempty exactly when velocities are present
	std::vector<std::uint32_t> positions;   // 3 per point
	std::vector<std::uint32_t> velocities;  // empty or 3 per point
	std::vector<std::uint32_t> radii;       // empty or 1 per point
	bool hasUniformRadius = false;          // exclusive with radii
	std::uint32_t uniformRadius = 0;        // binary32 bits, meaningful when hasUniformRadius
	std::vector<std::uint64_t> ids;         // empty or 1 per point
	std::vector<PointAttribute> attributes; // sorted by unique name
	std::size_t Count() const { return positions.size()/3; }
};

//! Header facts readable without decoding the arrays.
struct PointsHeader {
	std::uint32_t count = 0;
	bool ids = false, velocities = false, radii = false, uniformRadius = false;
};

// The budget covers retained input, the previous output, the candidate and
// validation scratch; counts are checked against the bytes present before
// anything is allocated.  Failure leaves the output untouched.  Encoding is
// canonical: every valid frame has exactly one encoding, flags are implied
// by the arrays present, and decode(encode(x)) == x.
bool EncodePoints(const PointsFrame& frame, std::size_t budget, std::vector<std::uint8_t>& out, std::string& error);
bool DecodePoints(const std::vector<std::uint8_t>& bytes, std::size_t budget, PointsFrame& out, std::string& error);
bool PeekPoints(const std::vector<std::uint8_t>& bytes, PointsHeader& out, std::string& error);

//! The field.v1 domain digest of per-point data: SHA-256 (lower-case hex) of
//! the little-endian id words when the frame has ids (stable across frames),
//! else of the position words (it then names exactly one frame).
std::string PointsDomain(const PointsFrame& frame);

} }
#endif
