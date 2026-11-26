// Versioned native simulation triangle-mesh payloads ("mesh.v1", "mesh.v2")
// and the static topology record ("mesh.topology.v1") a mesh.v2 frame may
// reference instead of carrying its connectivity. See docs/SIMULATION_MESH_FORMAT.md.
#ifndef RISE_SIMULATION_MESH_H
#define RISE_SIMULATION_MESH_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RISE { namespace Simulation {

//! Static payload type of the mesh.v2 topology record.
constexpr const char* MeshTopologyStaticType = "mesh.topology.v1";

//! A named texture-coordinate set: two binary32 samples (u, v) per vertex.
//! `units` states what the coordinates measure ("1" for a normalised chart,
//! "m" for material coordinates in metres).
struct MeshUVSet {
	std::string name, units;
	std::vector<std::uint32_t> values;
};

//! A named per-vertex attribute: `components` binary32 samples per vertex
//! (1 to 16), with units ("1" when dimensionless).
struct MeshAttribute {
	std::string name, units;
	std::uint8_t components = 1;
	std::vector<std::uint32_t> values;
};

// One triangle surface at one time. Samples are opaque IEEE binary32 bit
// strings, xyz interleaved per vertex, exactly as the grid codec stores them.
// `positionUnits` is the length unit of the frame (positions are lengths).
// uvSets and attributes are mesh.v2 only; each list is sorted by name and
// names are unique within it.
struct MeshFrame {
	std::string positionUnits = "m";
	std::string velocityUnits;            // nonempty exactly when velocities are present
	std::vector<std::uint32_t> positions;  // 3 per vertex
	std::vector<std::uint32_t> velocities; // empty or 3 per vertex
	std::vector<std::uint32_t> normals;    // empty or 3 per vertex (unitless)
	std::vector<std::uint32_t> indices;    // 3 per triangle, counter-clockwise front face
	std::vector<MeshUVSet> uvSets;         // mesh.v2: each 2 per vertex
	std::vector<MeshAttribute> attributes; // mesh.v2: each `components` per vertex
};

// Header facts readable without decoding the arrays. The topology digest is
// the SHA-256 (lower-case hex) of the little-endian words (V, T, 3T indices)
// -- in mesh.v1 exactly the wire bytes from the vertex count through the last
// index -- so two frames share connectivity exactly when their digests match,
// whatever the version and whether the indices are inline or in a static
// payload. `reference` is the content digest of the static topology payload a
// mesh.v2 frame references (empty when the connectivity is inline). Peek
// trusts the declared digests; DecodeMesh verifies them.
struct MeshTopology {
	std::uint32_t vertices = 0, triangles = 0;
	std::string digest;
	std::string reference;
	unsigned version = 1;
};

//! What mesh.v2 moves out of the frame into a static payload: the
//! connectivity, and the named UV sets that never change (those must exist
//! in the frame). `staticUVSets` requires `topology`.
struct MeshStaticLayout {
	bool topology = false;
	std::vector<std::string> staticUVSets;
};

//! Resolves a static payload by content digest with the reader's
//! verification and the remaining byte budget. False (with an error) when
//! the payload is undeclared, missing, corrupt or over budget.
using StaticPayloadResolver = std::function<bool(const std::string& digest, std::size_t budget,
	std::shared_ptr<const std::vector<std::uint8_t>>& bytes, std::string& error)>;

// The budget covers retained input, the previous output, the candidate and
// validation scratch. Failure leaves output untouched. Encoding is canonical:
// every valid MeshFrame has exactly one mesh.v1 encoding and, for a given
// layout, one mesh.v2 encoding; decode(encode(x)) == x.
// mesh.v1 refuses frames with UV sets or attributes.
bool EncodeMesh(const MeshFrame& frame, std::size_t budget,
	std::vector<std::uint8_t>& out, std::string& error);
// mesh.v2. With layout.topology the frame references `staticPayload`
// (a mesh.topology.v1 record, returned with its SHA-256 `staticDigest`,
// which is its cache content digest); otherwise both come back empty.
bool EncodeMeshV2(const MeshFrame& frame, const MeshStaticLayout& layout, std::size_t budget,
	std::vector<std::uint8_t>& out, std::vector<std::uint8_t>& staticPayload, std::string& staticDigest,
	std::string& error);
// A fixed-topology stream encodes its static record and hashes once, not per frame:
// PrepareMeshStatic fills `record` from a frame (layout.topology required); the
// overload below then writes frames byte-identical to the one above for any frame
// with the same connectivity and static UV sets, without re-encoding or hashing them.
struct MeshStaticRecord {
	std::vector<std::uint8_t> payload;  // the mesh.topology.v1 record
	std::string digest;                 // its SHA-256 (the cache content digest)
	std::string topologyDigest;         // MeshTopologyDigest of the frame's connectivity
};
bool PrepareMeshStatic(const MeshFrame& frame, const MeshStaticLayout& layout, std::size_t budget,
	MeshStaticRecord& record, std::string& error);
bool EncodeMeshV2(const MeshFrame& frame, const MeshStaticLayout& layout, std::size_t budget,
	const MeshStaticRecord& record, std::vector<std::uint8_t>& out, std::string& error);
// mesh.v1 or a self-contained mesh.v2 frame; a frame that references a static
// payload is refused without a resolver.
bool DecodeMesh(const std::vector<std::uint8_t>& bytes, std::size_t budget,
	MeshFrame& out, std::string& error);
// Any mesh.v1 / mesh.v2 frame; referenced static payloads come from `resolve`,
// are charged to the same budget and are verified (content digest by the
// resolver, topology digest, counts and UV-set names here).
bool DecodeMesh(const std::vector<std::uint8_t>& bytes, std::size_t budget, const StaticPayloadResolver& resolve,
	MeshFrame& out, std::string& error);
bool PeekMeshTopology(const std::vector<std::uint8_t>& bytes, MeshTopology& out, std::string& error);
// Digest the encoder would record for these indices (no other validation).
std::string MeshTopologyDigest(std::uint32_t vertices, const std::vector<std::uint32_t>& indices);
// Static payload digests a mesh payload references (none for mesh.v1).
bool MeshPayloadReferences(const std::vector<std::uint8_t>& bytes, std::vector<std::string>& digests,
	std::string& error);

} }
#endif
