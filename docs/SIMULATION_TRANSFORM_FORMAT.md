# Native rigid-transform payload v1

`transform.v1` (2026-11-23, simulation M6): a set of named rigid bodies at one time — position, orientation, linear
and angular velocity per body. It is the cache output of the rigid-body solver `rigid.bodies` (output `bodies`) and
the payload of its coupling port `body_state`; the renderer's `standard_object { transform_source sim.bodies[name] }`
reads it. It follows the rules of [`grid.v1`](SIMULATION_GRID_FORMAT.md), [`mesh.v1`](SIMULATION_MESH_FORMAT.md) and
[`field.v1`](SIMULATION_FIELD_FORMAT.md): little endian, packed, no trailing bytes, canonical, bounded decode. Code:
`src/Library/Simulation/SimulationTransform.{h,cpp}`; tests: `tests/SimulationTransformTest.cpp` (golden fixture
written independently with Python `struct`).

| Record | Fields, in byte order |
|---|---|
| Header | Eight bytes `RISETRFM`, u32 version = 1 |
| Units | text: length units of positions (velocities are those units per second) |
| Time | binary64: the frame's own time stamp, seconds (a cache frame repeats its manifest time) |
| Count | u32 body count N |
| Body × N | text name; 13 × binary64: position x y z, orientation w x y z, velocity x y z, angular velocity x y z |

Text is u16 byte length followed by the bytes (nonempty NFC UTF-8 without NUL).

- **Frames.** The body frame maps body coordinates `b` to world `x = R(q) b + position`. For `rigid.bodies` the
  position is the centre of mass and the body axes are the principal axes (analytic shapes keep their authoring
  axes). `velocity` is the velocity of the body origin; `angular velocity` is in the world frame (rad/s).
- **Orientation** is a unit quaternion `(w, x, y, z)` with `|1 − |q|| ≤ 1e-9` and the canonical sign: `w > 0`, or
  `w = 0` and the first nonzero of `x, y, z` positive. The encoder refuses any other quaternion rather than changing
  it, so every valid frame has exactly one encoding. `CanonicalQuaternion`, `QuaternionMatrix` and
  `MatrixQuaternion` are the helpers.
- **Bodies** are strictly increasing by unsigned UTF-8 name bytes; an empty set is valid.
- **Numbers** are finite binary64 (motion blur and long runs need more than binary32).
- **Bounds.** The body count is checked against the bytes present (each body takes at least 107 bytes) before
  allocating; the caller's budget covers retained input, the previous output and the candidate; failure leaves the
  output untouched.
- **Per-body port data.** A `field.v1` payload over the body set (the rigid solver's `body_load`) uses the domain
  `TransformBodyDomain(sorted names)` = SHA-256 of canonical CBOR `["rise.simulation.bodies.v1", [name...]]`, with
  one element per body in name order.
- **Cache.** `transform.v1` is a native payload type (`IsNativePayloadType`); it references no static payloads.
