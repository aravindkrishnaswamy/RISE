// Versioned native simulation triangle-mesh payloads ("mesh.v1").
// See docs/SIMULATION_MESH_FORMAT.md.
#ifndef RISE_SIMULATION_MESH_H
#define RISE_SIMULATION_MESH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

// One triangle surface at one time. Samples are opaque IEEE binary32 bit
// strings, xyz interleaved per vertex, exactly as the grid codec stores them.
struct MeshFrame {
	std::string positionUnits = "m";
	std::string velocityUnits;            // nonempty exactly when velocities are present
	std::vector<std::uint32_t> positions;  // 3 per vertex
	std::vector<std::uint32_t> velocities; // empty or 3 per vertex
	std::vector<std::uint32_t> normals;    // empty or 3 per vertex (unitless)
	std::vector<std::uint32_t> indices;    // 3 per triangle, counter-clockwise front face
};

// Header facts readable without decoding the arrays. The topology digest is
// the SHA-256 (lower-case hex) of the wire bytes from the vertex count through
// the last index, so two frames share connectivity exactly when their digests
// and vertex counts match. Peek trusts the declared digest; DecodeMesh verifies it.
struct MeshTopology {
	std::uint32_t vertices = 0, triangles = 0;
	std::string digest;
};

// The budget covers retained input, the previous output, the candidate and
// validation scratch. Failure leaves output untouched. Encoding is canonical:
// every valid MeshFrame has exactly one encoding and decode(encode(x)) == x.
bool EncodeMesh(const MeshFrame& frame, std::size_t budget,
	std::vector<std::uint8_t>& out, std::string& error);
bool DecodeMesh(const std::vector<std::uint8_t>& bytes, std::size_t budget,
	MeshFrame& out, std::string& error);
bool PeekMeshTopology(const std::vector<std::uint8_t>& bytes, MeshTopology& out, std::string& error);
// Digest the encoder would record for these indices (no other validation).
std::string MeshTopologyDigest(std::uint32_t vertices, const std::vector<std::uint32_t>& indices);

} }
#endif
