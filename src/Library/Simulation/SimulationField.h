// Versioned native payload for fields on a discrete element set ("field.v1"):
// named channels, each with units and a component count, over an index space
// identified by a domain digest (for per-vertex data on a mesh, the mesh's
// topology digest, SimulationMesh.h). Used for coupling-port payloads such as
// a per-vertex traction on a cloth sheet. See docs/SIMULATION_FIELD_FORMAT.md.
#ifndef RISE_SIMULATION_FIELD_H
#define RISE_SIMULATION_FIELD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

struct FieldChannel {
	std::string name, units;
	std::uint8_t components = 1;          // 1 to 16
	std::vector<std::uint32_t> values;    // binary32 bits, count x components, interleaved per element
};

struct FieldFrame {
	std::string domain;                   // 64 lower-case hex digits naming the index space
	std::uint32_t count = 0;              // elements in the index space
	std::vector<FieldChannel> channels;   // at least one, sorted by unique name
};

// The budget covers retained input, the previous output, the candidate and
// validation scratch. Failure leaves output untouched. Encoding is canonical.
bool EncodeField(const FieldFrame& frame, std::size_t budget, std::vector<std::uint8_t>& out, std::string& error);
bool DecodeField(const std::vector<std::uint8_t>& bytes, std::size_t budget, FieldFrame& out, std::string& error);
// Header facts without decoding the channels.
bool PeekFieldDomain(const std::vector<std::uint8_t>& bytes, std::string& domain, std::uint32_t& count,
	std::string& error);

} }
#endif
