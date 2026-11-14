# Release notes

## C++ utility modernization, unreleased

Regenerate photon maps, irradiance caches, serialized meshes, and other
binary assets written through RISE’s scalar serializers by the historical
Xcode `rise` CLI target. That
target defined `RISE_BIG_ENDIAN` on little-endian hosts. Affected writer
branches lost bits and exchanged double halves, so those files can contain
irrecoverable data loss. Regenerate them from the original scenes and
source assets with the corrected build.

- 3DS import accepts older or missing version metadata, skips empty mesh
  objects, skips malformed children within bounded non-mesh objects, and
  keeps previously discovered meshes when trailing metadata is
  malformed, including inside `EDIT3DS`.
  Consumed mesh records retain length and index checks. Malformed prefixes
  or short MAIN lengths before the mesh section are rejected; bytewise
  marker recovery is no longer performed.
- Disk buffers retain 64-bit size, cursor, and seek state. Photon-map saves
  stage their output and preserve the previous file on serialization or
  publication failure. Legacy 32-bit buffer queries saturate at `UINT_MAX`.
  Render output and viewport Save As permit direct encoding after denied or
  unsafe staging parents/opens, or through dangling output symlinks. Existing image files receive completed
  staged bytes in place to preserve inode, hardlinks, ownership, ACLs and
  extended attributes; new paths use rename, with completed-stage copying
  on eligible permission/EBUSY failures. Publication failure retains the
  complete staged image for recovery and logs its path and cause. POSIX
  staging directories are private from creation; staging leaves are created
  exclusively rather than following pre-existing entries. POSIX staging
  checks canonical ancestry: root/current-user ownership, sticky protection
  where foreign write is allowed, and no extended ALLOW ACLs on macOS.
  Unsafe ancestry fails required photon staging before encoding; image
  outputs can write directly there. Caller ACLs are not modified.
  Existing output files need write access; read permission is not required.
  Atomic-helper opens validate the opened regular file before truncating;
  a concurrent FIFO replacement cannot block POSIX publication. Direct
  buffer construction retains its previous stream/device behavior.
  Required photon saves keep atomic replacement and may change file identity
  and metadata. Residual staging paths are reported when cleanup is denied.
  Other staging errors remain failures. In-place copying or direct encoding
  can leave a partial image; failed copying retains its completed stage.
  Callers must serialize in-place writes to the same file or hardlink alias.
- POSIX staging ancestry is validated from `/` downward through opened
  directory descriptors, preventing mixed-tree validation. PNG and HDR10 PNG
  codec failures now propagate after cleanup, so partial headers do not
  count as successful images. Agent PNG helpers contain these failures.
  Null writer factories and disabled PNG encoders report failure by exception;
  viewport saves reject empty frames before opening or changing output.
- Spectral and piecewise-function tables accept their leading finite
  columns, skip file headers and malformed rows, and ignore extra columns.
  Inline control points require two whitespace-separated values: replace
  `cp 550,1.0` with `cp 550 1.0`, and supply an explicit amplitude for
  `cp 380` (the old parser silently used zero). Unequal spectral sample counts
  remain errors.
- Console commands support a standalone trailing `#` comment token and
  full-line `#` comments. Hash-prefixed values such as `#3` and `#Dir` stay
  literal; options and public tokenizer calls retain their literal hashes.
  Checked descriptor and command conversions reject malformed and
  out-of-range numbers. Camera keyframes require complete finite values.
  The parser guide records the accepted prefix and sign behavior.
- JPEG inputs above `INT_MAX` bytes and 3DS inputs above `UINT_MAX` bytes
  are explicitly rejected before reading payloads; these readers have
  narrower format/library limits than the disk buffer.
- TGA output rejects dimensions outside 1–65,535 before writing a header;
  The checked total-buffer budget is `UINT_MAX` bytes including the header;
  the CLI has a tighter raster budget and checks before allocating its source
  raster or opening the output. Existing output survives invalid dimensions.
  TGA import reads the complete header, IDs, BGRA channels and both origin
  axes correctly. Supported input is uncompressed 32-bit truecolor with
  straight alpha (or opaque padding when no alpha bits are declared);
  unused color maps are skipped; indexed-color decoding and extended TGA 2.0
  metadata interpretation remain unsupported. Alpha and orientation can
  therefore differ from the former shifted-header decode.
- Normalized integer output saturates RGBA before casting, including
  out-of-range/nonfinite alpha, while preserving truncation for valid samples.
  Floating EXR values and the raw scaling helper are unchanged.
- HDR/RGBEA shared-exponent output bounds converted channels before packing,
  preventing nonfinite casts and exponent wrap. Negative/NaN channels become
  zero; positive infinity and values beyond the format ceiling saturate.
  RGBEA preserves independently normalized alpha on black pixels. Exact
  power-of-two mantissa scaling also corrects HDR's legacy rounding factor,
  which dimmed power-of-two samples and could make raw pixels look like an
  RLE header. Finite HDR sample bytes can change.
- 16-bit PNG gamma metadata now follows the selected sample encoding, using
  the same approximate `gAMA` values as 8-bit output. Full color-profile
  tagging is unchanged.
- JPEG, TGA, HDR, PNG, TIFF, and EXR reader reloads clear previous active
  images, publish dimensions only after success, and check pixel coordinates.
  EXR data errors, including swallowed final-chunk short reads, return failure
  with an inactive image. PNG/JPEG reloads restore their construction cursor.
  EXR embedded streams use offsets relative to their header. JPEG requires
  complete marker framing through EOI; stb still handles entropy decoding
  with its existing tolerance for damaged data in a closed stream.
  Standard associated EXR RGB is unassociated in double before color
  conversion, correcting repeated alpha attenuation and dark PNG/TGA
  conversions. Zero-alpha RGB remains undivided; EXR rewriting still
  premultiplies it to zero. RGB-only EXR export still composites over black.
  EXR RGB and plain luminance Y/YA retain FLOAT range and offset windows; YC/YCA uses the SDK's
  historical half-precision chroma reconstruction. Legacy HDR RLE accepts
  chained multibyte repeat counts, including zero low-order bytes.
  Template thumbnail writes validate opened handles and refuse symlinks,
  hardlinks and foreign-owned entries. POSIX uses a private per-user cache
  directory and ignores the legacy shared cache name. Canonical ancestry and
  macOS ACLs are validated; unsafe temp roots are refused. Returned paths
  are absolute, including with relative `TMPDIR`. Caches are reused only
  when their bytes match the generated default PNG. Corrupt owned entries
  are regenerated; foreign-owned entries are refused, and
  failed writes return no usable texture path. Failed initial PNG encoding
  is retried after allocation recovers.
- HDR XYZE and a repeat marker without a preceding pixel in its scanline
  are rejected rather than decoded as RGBE or read outside the row.
- TIFF decoding rejects damaged strips. The 8-bit associated-alpha
  conversion can lose precision when converted back to straight alpha
  (for example, 200 at alpha 128 can return 199). The older partial-decoding mode
  could report success while inserting opaque-black rows.
- Media existence checks use the native narrow filename convention shared
  with the existing CRT and stream opens. Native Windows verification of
  non-ASCII paths remains outstanding.
  Windows static-library consumers must link Advapi32 for cache owner checks;
  the repository projects include this dependency.
- `mediapath add` in scripts registers the authored path. Cylindrical object
  UV modification reads its size from the correct argument and requires a
  lowercase `x`, `y`, or `z` axis; other axes formerly produced degenerate UVs.
  Voronoi 2D and 3D painters accept the default `border none`; editor nodes
  can also be created without supplying a border. These corrections can change
  file resolution, UV mapping, and derived scene contents in existing scripts.
- Concrete implementation types changed layout during this series.
  Clients constructing or subclassing them need rebuilding; this includes
  `JPEGReader` and `PNGReader` (saved input cursors), plus
  `TGAReader`, `HDRReader`, `PNGReader`, and `TIFFReader`, which retain
  image height for coordinate bounds checks.

See [the modernization plan](CPP_UTILITY_MODERNIZATION_PLAN.md) for the
implementation checkpoint, measurements, and remaining platform gates.
