# Native mesh payload v1

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

Not in v1, left for a later version: per-vertex attributes beyond velocity and normal (UVs, thickness, stress),
quads or polygons, double-precision positions, and several surfaces in one frame (use several outputs).
