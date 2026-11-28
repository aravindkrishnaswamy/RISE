# Native particle payload v1

`points.v1` (2026-11-25, M7 prerequisites; [M7 design](SIMULATION_SOLVER_CONTRACT.md) §3, [results](SIMULATION_SOLVER_CONTRACT.md)):
one particle set at one time — positions, and optionally velocities, a radius (uniform or per point), stable ids
and named per-point float attributes with units. It is the codec of a liquid's `particles` output (marker and spray
particles) and of imported particle data; the renderer's `simulated_points_geometry` draws each point as a sphere.
It follows the rules of [`mesh.v1`](SIMULATION_MESH_FORMAT.md) and [`grid.v1`](SIMULATION_GRID_FORMAT.md): little
endian, packed, no trailing bytes, canonical, bounded decode, finite binary32 samples whose bit patterns are kept.
Code: `src/Library/Simulation/SimulationPoints.{h,cpp}`; tests: `tests/SimulationPointsTest.cpp` (two golden fixtures
written independently with Python `struct` and `hashlib`).

| Record | Fields, in byte order |
|---|---|
| Header | Eight bytes `RISEPNTS`, u32 version = 1, u32 flags: bit 0 ids, bit 1 velocities, bit 2 per-point radius, bit 3 uniform radius; other bits zero; bits 2 and 3 never both |
| Units | text length units (positions and radii); text velocity units when bit 1 |
| Count | u32 point count N |
| Uniform radius | when bit 3: one binary32 |
| Ids | when bit 0: N u64 ids, strictly increasing |
| Samples | 3N positions; 3N velocities (bit 1); N radii (bit 2) |
| Attributes | u16 count A, then per attribute (strictly increasing names): text name, text units, u8 components (1–16), N × components binary32 |

Text is u16 byte length followed by nonempty NFC UTF-8 without NUL.

- **Canonical form.** Flags are implied by the arrays present; velocity units are present exactly when velocities
  are; there is one encoding of every valid frame, and decode(encode(x)) = x.
- **Radius** is in the length units, finite and nonnegative with the sign bit clear (`-0` is refused). A frame
  carries a per-point radius, a uniform radius, or neither (the consumer chooses, e.g. the renderer's
  `radius` parameter). A zero radius is a point that renders as nothing.
- **Ids** give correspondence across frames (motion blur over several frames, per-particle port data). They are
  u64 so that an emitter that spawns about a million particles per frame cannot run out over a long bake.
- **Attributes** are interleaved per point; the names `position`, `velocity`, `radius` and `id` are reserved.
- **Empty set** (N = 0) is valid: a spray output between splashes.
- **Bounds.** N is checked against the bytes present (12 per point at least) and every array against the bytes
  remaining before anything is allocated; the caller's budget covers the retained input, the previous output, the
  candidate and NFC scratch; failure leaves the output untouched. `PeekPoints` reads the count and flags only.
- **Per-point port data.** The `field.v1` domain digest (`PointsDomain`) is the SHA-256 of the N little-endian
  u64 id words when ids are present (stable while the set is), else of the 3N position words (it then names one
  frame exactly).
- **Cache.** `points.v1` is a native payload type (`IsNativePayloadType`) of the disk cache and the in-memory
  publisher; it references no static payloads.

Changes from the sketch in the contract's Appendix A.2 (which was never implemented): typed optional records in
place of a generic channel list (the renderer needs to know position, velocity and radius without interpreting
names), a uniform-radius record, velocity units as in `mesh.v1`, and u64 ids. A static payload for a fixed id set
is not provided (the ids are 8 bytes per point against at least 12 of positions).

The fixtures were produced with:

```python
import struct, hashlib
def text(s): b = s.encode(); return struct.pack('<H', len(b)) + b
b = b'RISEPNTS' + struct.pack('<II', 1, 1|2|4) + text('m') + text('m/s') + struct.pack('<I', 2)
b += struct.pack('<2Q', 7, 42) + struct.pack('<6f', 1, 2, 3, -0.5, 0.25, 0) + struct.pack('<6f', 0.5, 0, 0, 0, -1, 2)
b += struct.pack('<2f', 0.01, 0.02) + struct.pack('<H', 2)
b += text('color') + text('1') + struct.pack('<B', 3) + struct.pack('<6f', 1, 0.5, 0, 0, 0, 1)
b += text('temperature') + text('K') + struct.pack('<B', 1) + struct.pack('<2f', 300, 350.5)
print(b.hex(), hashlib.sha256(struct.pack('<2Q', 7, 42)).hexdigest())
```
