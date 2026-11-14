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
  objects, and keeps previously discovered meshes when trailing metadata is
  malformed, including inside `EDIT3DS`.
  Consumed mesh records retain length and index checks.
- Disk buffers retain 64-bit size, cursor, and seek state. Photon-map saves
  stage their output and preserve the previous file on serialization or
  publication failure. Legacy 32-bit buffer queries saturate at `UINT_MAX`.
  Render output, viewport Save As, and thumbnails permit direct writes on
  staging or rename permission failures, including through dangling symlinks.
  Rename fallback copies the completed image without repeating encoding.
  Other staging errors remain failures. A failed direct write can
  leave a partial image.
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
- TIFF decoding rejects damaged strips. The older partial-decoding mode
  could report success while inserting opaque-black rows.
- Media existence checks use the native narrow filename convention shared
  with the existing CRT and stream opens. Native Windows verification of
  non-ASCII paths remains outstanding.
- `mediapath add` in scripts registers the authored path. Cylindrical object
  UV modification reads its size from the correct argument and requires a
  lowercase `x`, `y`, or `z` axis; other axes formerly produced degenerate UVs.
  Voronoi 2D and
  3D painters accept the default `border none`. These corrections can change
  file resolution, UV mapping, and derived scene contents in existing scripts.
- Concrete implementation types changed layout during this series.
  Clients constructing or subclassing them need rebuilding.

See [the modernization plan](CPP_UTILITY_MODERNIZATION_PLAN.md) for the
implementation checkpoint, measurements, and remaining platform gates.
