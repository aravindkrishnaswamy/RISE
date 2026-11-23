# Native grid payload v1

The cache output type is `grid.v1`. Distinct output payloads have their own format/version. The manifest supplies frame ordinal, physical time, output binding, byte size and SHA-256. All counts/words are little endian, fields are packed with no alignment or padding, and trailing bytes are invalid.

| Record | Fields, in byte order |
|---|---|
| Frame header | Eight bytes `RISEGRID`, u32 version = 1, u32 nonzero channel count |
| Channel | Text name, text units, u8 component count (1 or 3), three i32 domain origin coordinates, three u32 positive domain extents, 16 binary64 matrix entries, one binary32 background tuple, u32 block count, then blocks |
| Block | Three i32 origin coordinates, three u32 positive extents, u8 constant flag (0 dense, 1 uniform), activity bytes, binary32 tuple data |

Text is u16 byte length followed by nonempty UTF-8 NFC bytes without NUL. Use `1` for dimensionless units. Channels are strictly increasing by unsigned UTF-8 name bytes. Blocks are strictly increasing by lexicographic (x,y,z) integer origins, physically disjoint, and wholly contained in the channel's half-open integer box. The last cell coordinate on each axis must fit i32. Empty block lists are valid. Different valid partitions can describe the same field; content addressing hashes the actual wire representation. Writers that need reproducibility must use a deterministic partition.

Index coordinates are signed two's-complement. Integer indices locate sample centers in world coordinates. The affine transform is a finite nonsingular row-major matrix multiplying column vectors (x,y,z,1); its last row is (0,0,0,1). Scalar/vector samples are opaque IEEE 754 binary32 bit strings. Application-specific value ranges are validated by the consumer. Components of a vector appear in x,y,z order; tuples are interleaved. Missing cells have background value and inactive status.

Dense block cell ordinal is x + nx*(y + ny*z) relative to its origin. Activity occupies ceil(cells/8) bytes, LSB first, unused high bits zero. All cells carry tuple data regardless of activity. A uniform block stores a single activity byte (0 or 1) followed by one tuple, regardless of its extents. An active background cell and an inactive nonbackground cell are distinct states.

Decoding rejects malformed counts, arithmetic overflow, out-of-range boxes, overlaps, unsorted channels/blocks, invalid masks and truncated or extended payloads. Caller-supplied remaining budget bounds retained input, old output, candidate decode, overlap-validation scratch and logical dense expansion; the caller separately reserves other render/consumer allocations. Native decoding preserves bit patterns including negative zero.

The independent golden fixture in tests/SimulationGridTest.cpp fixes field ordering, endian and activity independently of production encoding. Wrong-frame integrity remains the responsibility of the enclosing manifest reader.

## Offline OpenVDB import

For an ordered foreign sequence, run `rise-cache-import --vdb CACHE INTERVAL name=units... -- FILE.vdb...`. The first frame time is zero, and the interval is in seconds. Declare every named grid's units (use `1` for dimensionless). The cache binds all channels under the `fields` output and records preview fidelity. Import hashes the declared interval, channel units, converter version and ordered SHA-256 digests of the original files. It verifies the digest of the private byte buffer that it actually decodes.

The worker checks file structure before passing that buffer to OpenVDB. The native import profile accepts seekable OpenVDB archives v223–225 with independent FloatGrid or Vec3fGrid trees of branching dimensions 5/4/3, built-in affine maps and full-width samples. It supports raw, ZIP and Blosc payloads with optional activity-mask compression. It understands fixed-size numeric metadata, strings, and the historical delayed-load index. It bounds metadata, counts tree nodes from actual child masks, checks aligned root origins and ordering, and verifies chunk lengths and repeated leaf masks against topology and archive offsets. Unknown serializations fail before decode. The worker caps a foreign source preimage at 1 GiB, the tree allocation estimate at 4 GiB and frame projection at 2 GiB. Existing sparse values retain their binary32 bits, including stored inactive nonbackground values.
