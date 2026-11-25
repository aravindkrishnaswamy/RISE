# Native field payload v1

`field.v1` (2026-11-23, solver contract freeze): named channels of samples over a discrete element set — the
vertices of a mesh, the particles of a set, the cells of an irregular grid — identified only by a **domain
digest** and an element count. It is the codec for coupling-port payloads that live on another solver's elements,
such as a per-vertex traction or pressure on a cloth sheet (`cloth.shell` port `surface_load`). It follows the
rules of [`grid.v1`](SIMULATION_GRID_FORMAT.md) and [`mesh.v1`](SIMULATION_MESH_FORMAT.md): little endian, packed,
no trailing bytes. Code: `src/Library/Simulation/SimulationField.{h,cpp}`; tests: `tests/SimulationFieldTest.cpp`
(independent golden fixture, Python `struct`).

| Record | Fields, in byte order |
|---|---|
| Header | Eight bytes `RISEFELD`, u32 version = 1 |
| Domain | 32-byte domain digest, u32 element count N |
| Channels | u16 channel count C (≥ 1); per channel: text name, text units, u8 components (1–16), N × components binary32 |

- **Domain.** The digest names the index space; a consumer refuses a payload whose domain or count is not its own
  (no resampling — that is a port adapter's job in the group host). For per-vertex data on a mesh the domain is the
  mesh's topology digest (`MeshTopologyDigest`, identical for `mesh.v1` and `mesh.v2`), so a field is bound to one
  connectivity; per-particle data would use the digest of the particle set's identity (`points.v1`, specified in
  the contract, not implemented).
- **Channels** are strictly increasing by unsigned UTF-8 name bytes; names and units are nonempty NFC text without
  NUL (`1` for dimensionless). Samples are opaque finite binary32 bit strings, interleaved per element, components
  in x, y, z order.
- **Bounds.** As for the other codecs: counts are checked against the bytes present before allocating; the caller's
  budget covers retained input, the previous output, the candidate and NFC scratch; failure leaves the output
  untouched. Every valid frame has exactly one encoding.
