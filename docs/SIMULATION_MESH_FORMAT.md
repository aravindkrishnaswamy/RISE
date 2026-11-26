# Native mesh payloads v1 and v2

Two cache output types: `mesh.v1` (below) and `mesh.v2` (UV sets, named per-vertex attributes, and a
connectivity that can live in a static payload; [section at the end](#mesh-v2)). Readers serve both; `mesh.v1`
caches written before v2 stay readable unchanged.

## mesh.v1

The cache output type is `mesh.v1`: one triangle surface at one time (cloth, a liquid surface, a collider with
velocity). It sits beside [`grid.v1`](SIMULATION_GRID_FORMAT.md) and follows its rules: the manifest supplies
ordinal, time, output, byte size and SHA-256; all words are little endian; fields are packed with no padding; trailing
bytes are invalid. Code: `src/Library/Simulation/SimulationMesh.{h,cpp}`; tests: `tests/SimulationMeshTest.cpp`.

| Record | Fields, in byte order |
|---|---|
| Header | Eight bytes `RISEMESH`, u32 version = 1, u32 flags (bit 0 velocities, bit 1 normals; other bits zero) |
| Units | Text position units; then text velocity units when bit 0 is set |
| Topology | 32-byte SHA-256 topology digest, u32 vertex count V, u32 triangle count T, 3T u32 vertex indices |
| Samples | 3V binary32 positions; 3V velocities when bit 0 is set; 3V normals when bit 1 is set |

Text is u16 byte length followed by nonempty NFC UTF-8 without NUL, as in `grid.v1`. Normals are unitless and carry
no units field. Samples are opaque IEEE binary32 bit strings, xyz interleaved per vertex; every sample must be
finite, and bit patterns (including negative zero) are preserved. Triangles list three distinct in-range indices,
counter-clockwise when seen from the front. Unreferenced vertices are allowed. An empty mesh (V = T = 0) is valid,
for a changing-topology stream that is momentarily empty.

**Topology digest.** SHA-256 of the wire bytes from the vertex count through the last index. Two frames share
connectivity exactly when their digests match, so a reader detects a fixed-topology stream (cloth) by comparing a
header field, without decoding (`PeekMeshTopology`). The decoder recomputes the digest and refuses a mismatch;
`PeekMeshTopology` alone trusts it.

**Canonical form.** Every valid frame has exactly one encoding: flags are implied by which arrays are present,
velocity units are present exactly when velocities are, and there is nothing optional in the layout beyond those two
arrays. Re-encoding a decoded frame reproduces the input bytes.

**Bounds.** The decoder checks every count against the bytes actually remaining before allocating, then charges
the caller's budget for the retained input, the previous output, the candidate and NFC scratch, as the grid codec
does. Malformed counts, out-of-range or repeated indices, nonfinite samples, unknown flags or versions, truncation and
trailing bytes are refused. Failure leaves the output untouched.

The independent golden fixture in `tests/SimulationMeshTest.cpp` (assembled with Python `struct` and `hashlib`) pins
field order, endianness and the digest region independently of the encoder.

Not in v1: per-vertex attributes beyond velocity and normal (v2 adds UV sets and attributes), quads or polygons,
double-precision positions, and several surfaces in one frame (use several outputs).

<a name="mesh-v2"></a>
## mesh.v2 and the static topology record mesh.topology.v1

`mesh.v2` (2026-11-23, solver contract freeze) adds named UV sets, named per-vertex float attributes with units,
and lets a fixed-topology stream store its connectivity once: the indices (and any UV sets that never change) go
into a **static payload** of type `mesh.topology.v1`, written once per cache and referenced from every frame by
its SHA-256. Static payloads are a generic cache feature (declared in the manifest, immutable, content-addressed,
verified and budgeted like frames; [contract §Cache](SIMULATION_SOLVER_CONTRACT.md#cache)), not a mesh special case.
A changing-topology stream (M7) simply references a different topology record per frame, or carries its indices
inline. Code: `src/Library/Simulation/SimulationMesh.{h,cpp}` (`EncodeMeshV2`, `DecodeMesh` with a
`StaticPayloadResolver`); tests: `tests/SimulationMeshTest.cpp` (independent golden fixtures for both records).

| Record | Fields, in byte order |
|---|---|
| Header | `RISEMESH`, u32 version = 2, u32 flags (bit 0 velocities, bit 1 normals, bit 2 static topology; other bits zero) |
| Units | Text length units (the unit of positions); text velocity units when bit 0 is set |
| Topology | 32-byte topology digest, u32 vertex count V, u32 triangle count T; then, if bit 2, the 32-byte SHA-256 of the `mesh.topology.v1` payload, else 3T u32 indices |
| Samples | 3V positions; 3V velocities (bit 0); 3V normals (bit 1) |
| UV sets | u16 count U, then per set: text name, text units, 2V binary32 (u, v) |
| Attributes | u16 count A, then per attribute: text name, text units, u8 components (1–16), V × components binary32 |

`mesh.topology.v1` (static payload): `RISEMTOP`, u32 version = 1, u32 V, u32 T, 3T u32 indices, u16 count S of
static UV sets, then per set: text name, text units, 2V binary32.

- **Topology digest** is the same in both versions: SHA-256 of the little-endian words (V, T, 3T indices) — in
  `mesh.v1` exactly the wire region it already hashed — so a v1 and a v2 frame of the same connectivity have the
  same digest, `PeekMeshTopology` reads it without decoding, and the decoder recomputes it (from the inline indices
  or from the resolved record) and refuses a mismatch. It is also the domain digest of per-vertex `field.v1` port
  payloads ([format](SIMULATION_FIELD_FORMAT.md)).
- **Resolution.** A frame with bit 2 decodes only through a resolver (`DecodeMesh(bytes, budget, resolve, …)`);
  the cache snapshot's resolver reads the declared static payload and verifies its SHA-256 like a frame, the
  in-memory publisher serves the published bytes, a port exchange serves offered ones. The record is charged to the
  same decode budget; its V and T must equal the frame's; its UV-set names must not repeat a frame UV-set name.
- **UV sets** carry units: `1` for a normalised chart, `m` for material coordinates (cloth publishes its rest-space
  coordinates as the set `material`, in metres). The decoded frame lists static and frame UV sets merged, sorted
  by name. The renderer (`simulated_mesh_geometry`) uses the first set, or the one `MeshSourceOptions::uvSet`
  names, as texture coordinates, taken from the bind frame.
- **Attributes** are sorted by unique name; values are finite binary32, interleaved per vertex.
- **Canonical form.** Given the layout (which UV sets are static, whether the topology is static), every frame has
  one encoding; flags are implied by the arrays present; lists are strictly sorted; trailing bytes, unknown flags,
  out-of-range or repeated indices, nonfinite samples, components outside 1–16 and hostile counts are refused
  before allocating, as in v1.
- **Empty mesh** (V = T = 0) remains valid.
